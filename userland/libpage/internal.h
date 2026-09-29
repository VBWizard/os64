#ifndef PAGE_INTERNAL_H
#define PAGE_INTERNAL_H

#include "os64/mem.h"
#include "os64/str.h"
#include "page/page.h"

#define P_ARRAY(a) ((int32_t)(sizeof(a) / sizeof((a)[0])))

// Immutable strings live in page-owned arena blocks. Growable model vectors,
// retained selection defaults and replaceable edits own separate allocations.
// Submission scratch has its own arena so request construction does not
// change the page or extend the lifetime of temporary entry-list bytes.
typedef struct PBlock PBlock;

typedef struct {
    PBlock *blocks;
} PArena;

void *p_arena_alloc(PArena *arena, size_t size);
char *p_arena_copy(PArena *arena, const char *s, size_t n);
void p_arena_free(PArena *arena);

// Normalized defaults survive edits. Reset restores these without allocating.
typedef struct {
    const char *value;
    size_t value_len;
    bool checked;
    uint8_t *selected;
} PInitial;


// ── A PERSON'S EDIT, KEYED BY THE NODE IT BELONGS TO ────────────────────
//
// Never by an index, so a model rebuilt over a changed tree can re-key what
// survives (LIBPAGE.md ruling 2). This table is the only STORAGE of an
// edit; a control's `value`, `checked` and an option's `selected` are made
// to point at it, so a face still reads the live answer in one place.
//
// A record may reserve storage without carrying a change. Its text, on and
// chosen sentinels determine dirty state; record presence does not.

typedef struct {
    const os64_html_node_t *node;   // THE KEY
    char *text;                     // TEXT: NUL-terminated; NULL = untouched
    size_t text_len;
    int8_t on;                      // CHECKBOX / RADIO: -1 untouched, 0, 1
    uint8_t *chosen;                // SELECT, per option: 2 untouched, 0, 1
    int32_t nchosen;
} PEdit;

// Node to index, and string to index. Open addressing over a power-of-two
// table: the model is walked once and asked many times, and a face asks per
// node while it draws.
typedef struct {
    const void **keys;
    int32_t *vals;
    size_t cap, count;
} PPtrMap;

// A name to the NODE it names, because that is what both users want: a
// fragment wants somewhere to scroll to, and a `form=` wants an element to
// ask whether it is a form.
typedef struct {
    const char **keys;
    const void **vals;
    size_t cap, count;
} PStrMap;

struct os64_page {
    const os64_html_document_t *doc;
    os64_page_options_t opt;
    PArena arena;
    bool incomplete;

    // Where the page came from, and what references resolve against. Two
    // addresses, two jobs: the first decides a downgrade, the second
    // decides an address, and `<base href>` moves only the second.
    const char *document_url;
    const char *base_url;
    os64_url_t document, base;
    bool document_hierarchical, base_hierarchical;
    char document_scheme[OS64_URL_SCHEME_MAX];

    os64_page_link_t *links;
    int32_t nlinks, linkcap;
    os64_page_form_t *forms;
    int32_t nforms, formcap;
    os64_page_control_t *controls;
    int32_t ncontrols, controlcap;
    os64_page_image_t *images;
    int32_t nimages, imagecap;
    os64_page_background_t *backgrounds;
    int32_t nbackgrounds, backgroundcap;
    os64_page_sheet_t *sheets;
    int32_t nsheets, sheetcap;

    PInitial *initial;             // per control, captured after group normalization
    PEdit *edits;
    int32_t nedits, editcap;

    PPtrMap link_map, form_map, control_map, edit_map, image_map, background_map;
    // A fragment matches an `id` FIRST and an old-style `<a name>` second,
    // whatever tree order says, so the two cannot share one table.
    PStrMap id_map, aname_map;
    // A radio group is one name under one owner, and the members have to be
    // findable from any one of them: to untick the rest, and to ask whether
    // a required group has anything ticked at all. The chain is over every
    // radio sharing a name; the owner is compared when it is walked.
    PStrMap radio_map;
    int32_t *radio_next;            // per control; -1 ends the chain
    // ONE GROUP, LINKED: its first member (a control's own index when it is
    // in no group) and the next member of the same name AND owner, -1 at the
    // end. Built once, so a question about a whole group — validation's
    // "is anything ticked?" — costs its size and not its size times itself.
    int32_t *group_head;
    int32_t *group_next;
    // Per group head: the first member validation applies to, where the
    // group is judged so that failures still come out in tree order; -1
    // when every member is barred.
    int32_t *group_judged_at;

    // The first VALID declarative refresh in tree order. An invalid pragma
    // sets nothing, which is what leaves a later one free to win.
    os64_page_refresh_t refresh;
    bool has_refresh;
};

// ── The arena and the vectors ───────────────────────────────────────────

void *p_alloc(os64_page_t *page, size_t size);
char *p_copy(os64_page_t *page, const char *s, size_t n);
// Room for one more item in a heap vector, doubling. False on no memory,
// which every caller turns into `incomplete` rather than into nothing.
bool p_grow(void **items, int32_t *cap, int32_t count, size_t size);

bool p_ptrmap_put(PPtrMap *map, const void *key, int32_t val);
int32_t p_ptrmap_get(const PPtrMap *map, const void *key);
void p_ptrmap_free(PPtrMap *map);
// FIRST WINS: an id or an anchor name that repeats keeps the earliest, which
// is what the standard's "the first such element" asks for.
bool p_strmap_put(PStrMap *map, const char *key, const void *val);
// The same, REPLACING what is there. For a chain whose head moves, where
// "first wins" would pin the head to whatever went in first.
bool p_strmap_set(PStrMap *map, const char *key, const void *val);
const void *p_strmap_get(const PStrMap *map, const char *key);
void p_strmap_free(PStrMap *map);

// ── Reading the tree ────────────────────────────────────────────────────

// An attribute's value, or NULL when the element does not carry it. The
// empty string is a value: `formaction=""` is the page's own address.
const char *p_attr(const os64_html_node_t *n, const char *name);
bool p_has_attr(const os64_html_node_t *n, const char *name);
bool p_is(const os64_html_node_t *n, os64_html_tag_t tag);
// An HTML-namespace element of this tag anywhere up the tree, or NULL.
const os64_html_node_t *p_ancestor(const os64_html_node_t *n, os64_html_tag_t tag);
// A subtree's text in tree order, into the page's arena. `collapse` applies the
// standard's "strip and collapse ASCII whitespace", which is what an
// option's text is defined in terms of — so a list written across several
// lines of markup offers the same words a one-line one does. NULL on no
// memory. `len` may be NULL.
char *p_subtree_text(os64_page_t *page, const os64_html_node_t *n, bool collapse, size_t *len);
// The same into any arena, for a reader that must not write to the page.
char *p_subtree_text_into(PArena *arena, const os64_html_node_t *n, bool collapse, size_t *len);
// HTML non-negative integer prefix parsing, saturated on arithmetic overflow.
bool p_nonnegative(const char *text, uint64_t *value);
// Whether the select display size is one, using HTML integer parsing.
bool p_select_one_line(const os64_page_control_t *control);
// Make a control's `value`, `checked` and its options' `selected` agree with
// the edit table, which is where a person's changes are actually kept.
void p_publish(os64_page_t *page, int32_t control);
// The edit record for a control, made if it is not there yet. NULL on no
// memory. `p_edit_find` never makes one.
PEdit *p_edit_for(os64_page_t *page, int32_t control);
PEdit *p_edit_find(const os64_page_t *page, int32_t control);

// ── A. The base and reference resolution ────────────────────────────────

// Where the page came from, canonicalised. False on no memory.
bool p_document_url(os64_page_t *page, const char *text);
// What references resolve against: the first `base` with an `href`, else the
// document's own address. False on no memory.
bool p_base(os64_page_t *page);
// Resolve one reference against the page's base, splitting off the `#name`
// the address itself cannot carry. `ref` NULL means the page spelled none.
void p_resolve(os64_page_t *page, const char *ref, os64_page_ref_t *out);
void p_resolve_action(os64_page_t *page, const char *ref, os64_page_ref_t *out);
// Canonicalise an address that is already absolute: the scheme's default
// port is never spelled, because a link nobody wrote must not appear.
bool p_canonical(os64_page_t *page, const char *text, const char **out,
                 os64_page_reason_t *refused);
// The port `scheme` implies, or 0 when this library does not know one. The
// only scheme policy in here; os64/url.h keeps grammar and declines policy.
uint16_t p_default_port(const char *scheme);
// Percent-decoding, in place over BYTES, so `%C3%A9` becomes the same UTF-8
// the page was written in. Returns the new length.
size_t p_percent_decode(char *s, size_t len);

// ── K. The navigation a document declares ───────────────────────────────

// The shared declarative refresh steps, over one `content` attribute: the
// delay, and the
// address if it named one. False means the page declared no refresh at all.
// `url` points INTO `input` and is not NUL-terminated.
bool p_refresh_content(const char *input, uint32_t *seconds, const char **url, size_t *url_len);
// One `meta`, asked whether it declares a refresh, and recorded on the page
// when it does. False leaves the page still looking.
bool p_refresh_from(os64_page_t *page, const os64_html_node_t *n);

// ── H. Value sanitization ───────────────────────────────────────────────

// HTML numeric syntax is strict for values, prefix-parsed for attributes.
bool p_number_parse(const char *text, bool strict, double *out);
// Shortest decimal form of a finite binary64 value; out has at least 32 bytes.
size_t p_number_spell(double value, char out[32]);
long double p_number_mod(long double x, long double y);
const char *p_range_value(PArena *arena, const os64_html_node_t *node,
                          const char *raw, size_t *len);
os64_page_input_t p_input_type(const os64_html_node_t *n);
// Shared by initial values and edits; temporary storage belongs to arena.
// raw_len is authoritative; raw also has a terminator at that offset.
const char *p_sanitize_value(PArena *arena, const os64_html_node_t *n,
                            os64_page_element_t element, os64_page_input_t input,
                            const char *raw, size_t raw_len, size_t *len);
// The value the standard says this control holds, with the page's own bytes
// sanitized for its type. Arena storage, or a tree pointer when nothing
// needed changing.
const char *p_page_value(os64_page_t *page, const os64_html_node_t *n,
                         os64_page_element_t element, os64_page_input_t input, size_t *len);

// ── C, D, I. The submitter, the entry list, validation ──────────────────

// One name and value on their way to the wire. Both are UTF-8 here; family
// E turns them into the form's selected encoding.
typedef struct {
    const char *name;
    size_t name_len;
    const char *value;
    size_t value_len;
    bool is_file;
} PEntry;

typedef struct {
    PEntry *items;
    int32_t count, cap;
    PArena arena;                   // the normalised bytes, and nothing of the page's
    // A `dir=auto` element's direction, by node, for this one list: every
    // `dirname` control under one such element would otherwise scan its
    // whole subtree again.
    PPtrMap direction;
} PEntries;

// The control a submission submits FROM, or -1 for a form submitting
// itself. `reason` says why when it answers false.
bool p_submitter(const os64_page_t *page, int32_t form, os64_page_what_t what,
                 int32_t *submitter, os64_page_reason_t *reason);
// What the submitter overrules, folded onto the form's own answers.
void p_effective(const os64_page_t *page, int32_t form, int32_t submitter,
                 const os64_page_ref_t **action, os64_page_method_t *method,
                 os64_page_enctype_t *enctype, bool *novalidate);
// Constraint validation. False and `*control` names the first failure.
bool p_validate(const os64_page_t *page, int32_t form, int32_t *control);
bool p_entry_list(const os64_page_t *page, int32_t form, int32_t submitter,
                  os64_page_what_t what, PEntries *out);
void p_entries_free(PEntries *entries);

// ── E. The encoding ─────────────────────────────────────────────────────

// The encoding the entry list goes out in: the first label in the form's
// `accept-charset` libhtml supports, else the document's, else UTF-8 —
// then the standard's output-encoding step, which turns UTF-16 into UTF-8.
const char *p_encoding_for(const os64_page_t *page, int32_t form);
// Serialise an entry list. `bytes` and `type` come back os64_malloc'd and
// belong to the caller, which is the request — the GET path takes the
// urlencoded bytes as a query and the POST path takes them as a body, so
// there is one serialiser and not one per method.
bool p_serialise(const PEntries *entries, os64_page_enctype_t enctype, const char *encoding,
                 size_t max_body, char **bytes, size_t *len, char **type,
                 os64_page_reason_t *refused);

#endif // PAGE_INTERNAL_H
