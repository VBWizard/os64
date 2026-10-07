#ifndef OS64_HTML_H
#define OS64_HTML_H

#include "html/tags.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct os64_html_parser os64_html_parser_t;

typedef enum {
    OS64_HTML_OK = 0,
    /* Not a refusal: the parse is stopped at a script (below). */
    OS64_HTML_SCRIPT = 1,
    OS64_HTML_TOO_LARGE = -1,
    OS64_HTML_ARENA_EXHAUSTED = -2,
    OS64_HTML_NO_MEMORY = -3,
    OS64_HTML_TOO_DEEP = -4,
    OS64_HTML_WORK_EXHAUSTED = -5,
    /* What a verb that changes a document answers (below). */
    OS64_HTML_HIERARCHY = -6,      /* the tree the DOM Standard refuses to make */
    OS64_HTML_NOT_FOUND = -7,      /* the reference child is not the parent's child */
    OS64_HTML_BAD_TEXT = -8,       /* not UTF-8, or it holds a NUL */
    OS64_HTML_ROOT_REQUIRED = -9,  /* the document would lose its html element */
    OS64_HTML_BAD_ARGUMENT = -10
} os64_html_status_t;

/* Exit badge for a broken node-hold or snapshot-pin lifetime contract,
 * including document teardown with outstanding holds or pins ("HTML"). */
#define OS64_HTML_FATAL_EXIT 0x48544D4C

typedef enum {
    OS64_HTML_DOCUMENT,
    OS64_HTML_FRAGMENT,
    OS64_HTML_DOCTYPE,
    OS64_HTML_ELEMENT,
    OS64_HTML_TEXT,
    OS64_HTML_COMMENT
} os64_html_node_kind_t;

typedef enum { OS64_HTML_NS_HTML, OS64_HTML_NS_SVG, OS64_HTML_NS_MATHML } os64_html_ns_t;

typedef enum { OS64_HTML_NO_QUIRKS, OS64_HTML_LIMITED_QUIRKS, OS64_HTML_QUIRKS } os64_html_quirks_t;

typedef struct {
    const char *charset;
    size_t max_bytes, max_arena_bytes, max_depth;
    uint64_t max_work;
    /* Parse as a browser that runs scripts does (THE PARSE THAT STOPS,
     * below). Off, a script is text like any other and no call stops. */
    bool scripting;
} os64_html_options_t;

typedef struct os64_html_attr {
    const char *name, *value;
    /* Namespace URI, or NULL. name retains the qualified spelling. */
    const char *ns;
    struct os64_html_attr *next;
} os64_html_attr_t;

/* Nodes and attributes are read-only views owned by their document; what
 * changes one is a verb, at the foot of this file. */
typedef struct os64_html_node {
    os64_html_node_kind_t kind;
    os64_html_ns_t ns;
    os64_html_tag_t tag;
    /* The library's mark of which document the node belongs to. It means
     * nothing to a reader. */
    uint32_t document_id;
    const char *name;
    const char *public_id, *system_id;
    const char *text;
    size_t text_len;
    os64_html_attr_t *attrs;
    struct os64_html_node *template_contents;
    struct os64_html_node *parent, *first_child, *last_child, *prev, *next;
    /* Parser association at insertion, or NULL; document-owned, possibly
     * non-ancestor. Consumers resolve form= and ancestry separately. */
    struct os64_html_node *form_owner;
} os64_html_node_t;

typedef struct {
    size_t byte_offset;
    const char *name;
} os64_html_parse_error_t;

typedef struct os64_html_document {
    os64_html_node_t *document, *html, *head, *body;
    os64_html_quirks_t quirks;
    int64_t refusal;
    bool truncated;
    /* charset is NULL if refusal prevented encoding selection. */
    const char *charset, *charset_unsupported, *charset_late_meta;
    /* node_count includes detached allocations; arena counts capacity/headers. */
    size_t node_count, arena_bytes, input_bytes;
    size_t peak_arena_bytes;
    uint64_t work;
    size_t parse_errors;
    os64_html_parse_error_t first_errors[16];
} os64_html_document_t;

/* NULL options select defaults. For overrides, initialize with options_default;
 * supplied zero limits are literal zero limits. The charset label is copied.
 * Constructor failure returns NULL, including a budget below initial storage
 * and a program that has begun 2^32 - 1 documents: each has a mark its nodes
 * carry, and a mark is not given out twice.
 * Each parser has independent state; serialize calls on the same parser. */
#pragma GCC visibility push(default)
os64_html_options_t os64_html_options_default(void);
os64_html_parser_t *os64_html_parser_new(const os64_html_options_t *options);
int64_t os64_html_parser_feed(os64_html_parser_t *p, const void *bytes, size_t len);
/* Consumes p. A successful constructor reserves a freeable partial document
 * and its html node, so finish does not need to allocate to transfer ownership. */
os64_html_document_t *os64_html_parser_finish(os64_html_parser_t *p);
void os64_html_parser_destroy(os64_html_parser_t *p);
void os64_html_document_free(os64_html_document_t *doc);

/* ── THE PARSE THAT STOPS (docs/design/pending/DOM.md) ────────────────────
 *
 * With `scripting` set, `noscript` is parsed as the standard says a browser
 * with scripting enabled parses it (its contents are text), and THE PARSE
 * STOPS AT THE END TAG OF EACH SCRIPT: the call answers OS64_HTML_SCRIPT,
 * which is not a refusal, and `os64_html_parser_script` names the element,
 * whole and where the parser put it. It names one until the parse is
 * resumed and NULL otherwise. Whether that script is run, and how, is the
 * caller's business; the parser only waits. An SVG `script` stops the parse
 * too, at its end tag or at a start tag that closes itself. A script inside
 * a template's contents, which no browser runs, does not, nor does one that
 * only the end of the input closed.
 *
 * `resume` carries on, and may stop at the next script. A `feed` while the
 * parse is stopped parses nothing: its bytes are kept on the heap, counted
 * against `max_bytes` and not against the arena, and it answers
 * OS64_HTML_SCRIPT again. Where a chunk ends stays invisible: stops fall at
 * the same scripts, and the document comes out the same, however the bytes
 * were cut. Which call answers a stop does depend on the cut. In particular
 * a parser reads its first 1024 bytes for an encoding before it parses any
 * of them, so a script among those stops the call that fills that window,
 * or the one that says the input has ended.
 *
 * THE END OF THE INPUT is said one of two ways. `finish` is the way that
 * does not stop: it parses whatever is kept and the end-of-input steps
 * straight through, past any script, and hands over the document. A host
 * that runs scripts says `end` first, which may stop as `feed` does, and
 * calls `finish` once `end` or a `resume` after it has answered
 * OS64_HTML_OK. No byte is fed after `end` (OS64_HTML_BAD_ARGUMENT, which
 * like a `resume` of a parse that is not stopped changes nothing).
 *
 * `abandon` is for a page being left mid-load: it parses nothing more, runs
 * no end-of-input steps, and hands over the document as far as it was
 * built, to be freed like any other. (`destroy` frees the document with the
 * parser, which is right when nothing can be pointing into it.)
 *
 * `write` IS document.write (docs/design/pending/DOM_D9.md). The host
 * calls it for the script the parse is stopped at, while that script runs
 * (which caller is writing is the host's to know; the parser knows only
 * that it is stopped). `len` bytes of UTF-8 go into the input just before
 * its next character and are parsed at once, as far as that point and no
 * further. The script's next write goes after everything it has written,
 * however the bytes around it were cut.
 * A script it wrote stops the parse as one fed would: the call answers
 * OS64_HTML_SCRIPT and `os64_html_parser_script` names the new one, and
 * the writer's later writes wait behind it. `resume` then means "the
 * script that was running is done": if what it wrote stopped the parse, it
 * answers OS64_HTML_SCRIPT at once and parses nothing, so the host runs
 * that script next, and its own writes go just after it. Written text is
 * INPUT and not tree: it is kept on the heap and counted against
 * `max_bytes`, which ends the input where it falls as it does for fed
 * bytes (a write is cut there on a character, not a byte): whatever was
 * waiting behind the cut is dropped unparsed, and nothing fed or written
 * after it is taken, a script before it still stops the parse; it moves the
 * version as any parsing call does, and an empty write changes nothing. A write that
 * is not UTF-8 or holds a NUL is OS64_HTML_BAD_TEXT and nothing of it is
 * parsed. A write when the parse is not stopped at a script (before the
 * first stop, after a `resume` or an `end` that answered OK) is
 * OS64_HTML_BAD_ARGUMENT and changes nothing. A refusal met while parsing
 * it is the parse's, as for `feed`.
 * `finish` parses what was written with the rest; `abandon` frees it.
 *
 * BETWEEN TWO CALLS THE TREE IS A DOCUMENT. `os64_html_parser_document`
 * answers it, the same one `finish` will hand over. It can be read, pinned
 * and changed with the verbs at the foot of this file, with one
 * difference from a finished document: until the parser has reached the
 * first tag or text, the `html` element is not yet in the document. The
 * parser still owns it: it is not freed while the parser lives.
 *
 * Every call that parses anything moves the document's version, and so does
 * a `finish` or an `abandon` that has to give the document its `html`
 * element. What a snapshot pinned between two calls keeps pointing at stays
 * put: the parser adding to a text node such a snapshot can see writes the
 * longer text elsewhere and retires the old bytes, as `set_text` does.
 *
 * A verb between two calls may leave the tree unlike anything a parser
 * built: its open elements detached, moved, nested the other way. The
 * parser then goes on as the standard says, adding to the elements it has
 * open wherever they now are, but it keeps the verbs' rules. A link the
 * rules refuse is not made and the node stays where it was (a cycle; a
 * second element, or a doctype after the element, under the document). A
 * node that would lie deeper than the limit refuses the parse
 * (OS64_HTML_TOO_DEEP). A control the parser moves out of its form's tree
 * loses its `form_owner`, and `head` and `body` stay the document element's
 * first of each. */
os64_html_node_t *os64_html_parser_script(const os64_html_parser_t *p);
int64_t os64_html_parser_resume(os64_html_parser_t *p);
int64_t os64_html_parser_write(os64_html_parser_t *p, const char *utf8, size_t len);
int64_t os64_html_parser_end(os64_html_parser_t *p);
os64_html_document_t *os64_html_parser_document(os64_html_parser_t *p);
os64_html_document_t *os64_html_parser_abandon(os64_html_parser_t *p);
const os64_html_attr_t *os64_html_attr(const os64_html_node_t *element, const char *name);
os64_html_tag_t os64_html_tag_from_name(const char *name);
const char *os64_html_tag_name(os64_html_tag_t tag);
const char *os64_html_status_name(int64_t status);

/* THE ENCODING TABLE ANSWERS FOR EVERYONE, so nothing carries a second copy
 * of it. A parser needs to read a label and decode bytes; a consumer asking
 * what a form's accept-charset names, and writing that encoding back out,
 * needs the same table read the other way.
 *
 * `os64_html_encoding_for_label` takes one label of `len` bytes, ASCII
 * whitespace at either end ignored, and answers with the canonical name of
 * the encoding it names — "utf-8", "windows-1252", "utf-16le", "utf-16be",
 * the four this parser decodes — or NULL for a label it does not know. The
 * name is a literal and outlives any document.
 *
 * `os64_html_encode_windows_1252` is the Encoding Standard's single-byte
 * encoder for that index. False means the encoding has no byte for the code
 * point, which is a fact and not an error: what to send instead belongs to
 * whoever is doing the sending. */
const char *os64_html_encoding_for_label(const char *label, size_t len);
bool os64_html_encode_windows_1252(uint32_t cp, uint8_t *out);

/* ── CHANGING A DOCUMENT (docs/design/pending/DOM.md) ─────────────────────
 *
 * The structs above stay read-only views. A change goes through a verb, so
 * that every one keeps what every reader relies on: no cycle, the DOM
 * Standard's rules for what may be whose child, strings that are UTF-8 with
 * no NUL, a document that still has its `html` element, and a tree no deeper
 * than a parse under the same `max_depth` could have built, which is twice
 * that number of levels (OS64_HTML_TOO_DEEP). A verb either happens or changes nothing a
 * reader can see, and answers OS64_HTML_OK or the name of why not; a verb's
 * failure is the verb's, and `doc->refusal` stays what the parse said.
 *
 * A REMOVED SUBTREE LIVES WHILE SOMETHING HOLDS IT. A successful remove,
 * replace, or insertion draining a fragment can reclaim the detached subtree
 * when no node in it is held and no older snapshot pin remains. A counted
 * hold preserves node identity across mutations; a pin preserves snapshot
 * bytes. Hold a node before a verb if it must remain usable afterwards.
 * A successful insertion consumes even an empty fragment container unless
 * it is held; an empty insertion still leaves the document version unchanged.
 * Creation results never inserted stay charged until document teardown.
 * What a verb allocates is charged to the document's `max_arena_bytes`.
 *
 * A VERB REPLACES A STRING; IT DOES NOT WRITE INTO ONE. Replacing a node's
 * text or an attribute's value swaps the pointer and RETIRES the old bytes,
 * which are freed once nothing built before the swap can still be pointing
 * at them.
 * The document learns that from PINS: whatever keeps pointers into the tree
 * across a change (a page model, a cascade, a layout) pins the document
 * while it does, and lets go when it is freed.
 *
 * `os64_html_version` moves on every change a reader could see, and is never
 * zero for a document: comparing it with the version something was built at
 * answers "has the tree moved since?". The library calls nobody. A verb that
 * leaves everything as it was answers OK and does not move it: an empty
 * fragment inserted, a node inserted where it already sits (unless that
 * parted a control from its form, below), a text or an attribute set to the
 * value it has, an absent attribute removed, a node replaced by itself.
 *
 * `os64_html_pin` answers a pin, or 0 when the document has none left to
 * give (it has sixteen). No two pins in a program are answered with the same
 * number, so a pin names one taking of it and nothing later. Freeing a
 * document while one is held, or releasing what is not held (a pin let go
 * already, or one taken on another document), ends the program
 * (OS64_HTML_FATAL_EXIT): the holder is about to read freed memory.
 * `os64_html_retired_bytes` counts individually owned blocks kept by pins,
 * including unheld retired subtrees. Permanent chunks remain charged.
 *
 * EVERY NODE A VERB IS HANDED BELONGS TO `doc`, the source of a clone
 * excepted. A node lies in its document's arena and can be reclaimed when
 * removed or when that document is freed, so a foreign node would point to
 * freed memory in waiting; a verb refuses it (OS64_HTML_BAD_ARGUMENT). */
typedef uint64_t os64_html_pin_t;
/* Allocation-free, counted identity protection. The document must own the
 * node; invalid ownership, overflow, and an unheld release end the program
 * with OS64_HTML_FATAL_EXIT. Releasing the last hold may reclaim a formerly
 * detached subtree, so the node must not be read after release. Document
 * teardown refuses outstanding holds as well as live snapshot pins. */
void os64_html_hold(const os64_html_document_t *doc, const os64_html_node_t *node);
void os64_html_release(const os64_html_document_t *doc, const os64_html_node_t *node);
uint64_t os64_html_version(const os64_html_document_t *doc);
/* Owned HTML input nodes carrying an unnamespaced form attribute. Includes
 * detached nodes and template contents, independent of input type/value.
 * NULL answers zero. Zero permits skipping explicit-owner scans; a nonzero
 * count does not establish that an affected radio is in the document tree. */
size_t os64_html_form_input_count(const os64_html_document_t *doc);
/* Whether a live node belongs to this document, connected or detached.
 * NULL for either argument answers false. This compares the library's
 * ownership mark; it neither walks the tree nor changes the document. */
bool os64_html_owns_node(const os64_html_document_t *doc, const os64_html_node_t *node);
os64_html_pin_t os64_html_pin(const os64_html_document_t *doc);
void os64_html_unpin(const os64_html_document_t *doc, os64_html_pin_t pin);
size_t os64_html_retired_bytes(const os64_html_document_t *doc);

/* New nodes, detached and owned by `doc`. NULL with `*status` (which may be
 * NULL) saying why. An element's `name` is taken as written: the caller
 * lowercases an HTML one. `os64_html_clone` copies one node, or with `deep`
 * its whole subtree and a template's contents; a copy has no form owner.
 * Its `node` may belong to another document, which is how a node is brought
 * from one to another: the copy then shares no memory with that document
 * and outlives it. A subtree taller than this document allows is refused
 * (OS64_HTML_TOO_DEEP). */
os64_html_node_t *os64_html_create_element(os64_html_document_t *doc, os64_html_ns_t ns,
                                           const char *name, int64_t *status);
os64_html_node_t *os64_html_create_text(os64_html_document_t *doc, const char *utf8, size_t len,
                                        int64_t *status);
os64_html_node_t *os64_html_create_comment(os64_html_document_t *doc, const char *utf8, size_t len,
                                           int64_t *status);
os64_html_node_t *os64_html_create_fragment(os64_html_document_t *doc, int64_t *status);
os64_html_node_t *os64_html_clone(os64_html_document_t *doc, const os64_html_node_t *node,
                                  bool deep, int64_t *status);

/* `insert` puts `node` under `parent` before `before` (NULL: last), moving
 * it from wherever it was; a FRAGMENT gives up its children in its place and
 * is left empty. `replace` puts `node` where `old` is. `remove` unlinks a
 * node that has a parent and is a no-op on one that has none. Adjacent text
 * is not merged. None of the three allocates.
 *
 * A document's element or doctype cannot be inserted under that document
 * again, even to where it already is: the rule that a document has one of
 * each counts the node itself (OS64_HTML_HIERARCHY), as it does in a
 * browser. Replacing either with itself is allowed and changes nothing.
 *
 * A form control's `form_owner` record is cleared by the change that makes
 * it untrue, as the HTML Standard's "reset the form owner" says: a move that
 * leaves the control and its form in different trees, whichever of the two
 * moved, and giving a listed control a `form` attribute of its own. A
 * control and its form that move together stay tied. */
int64_t os64_html_insert(os64_html_document_t *doc, os64_html_node_t *parent,
                         os64_html_node_t *node, os64_html_node_t *before);
int64_t os64_html_replace(os64_html_document_t *doc, os64_html_node_t *parent,
                          os64_html_node_t *node, os64_html_node_t *old);
int64_t os64_html_remove(os64_html_document_t *doc, os64_html_node_t *node);

/* An attribute by its qualified name, as os64_html_attr finds it. Setting
 * one that exists keeps its place in the list; a new one goes last.
 * `set_text` replaces a TEXT or COMMENT node's data whole. */
/* Changes are applied in order to one element's qualified attributes. A set
 * retains an existing record's namespace and position; a new name goes last.
 * A remove ignores value/value_len. All entries are validated before any
 * change; failure preserves the tree, version, parse verdict and arena
 * accounting, including peak. Complete success publishes once, with replaced
 * records retired for existing pins. An unchanged final list is a no-op. */
typedef struct {
    const char *name, *value;
    size_t value_len;
    bool remove;
} os64_html_attr_change_t;
int64_t os64_html_set_attrs(os64_html_document_t *doc, os64_html_node_t *element,
                            const os64_html_attr_change_t *changes, size_t count);

int64_t os64_html_set_attr(os64_html_document_t *doc, os64_html_node_t *element,
                           const char *name, const char *value, size_t value_len);
int64_t os64_html_remove_attr(os64_html_document_t *doc, os64_html_node_t *element,
                              const char *name);
int64_t os64_html_set_text(os64_html_document_t *doc, os64_html_node_t *node, const char *utf8,
                           size_t len);

/* Parse a complete UTF-8 string in an owned ELEMENT context. The returned
 * FRAGMENT is detached and owned by doc; script elements never stop or run.
 * scripting selects noscript's policy. There is no sniffing, BOM stripping,
 * or meta charset interpretation. Invalid UTF-8 becomes U+FFFD. NUL follows
 * tokenizer/tree state: ordinary HTML text drops it; raw text, RCDATA,
 * attributes, comments and foreign text replace it.
 * Work is capped at saturating (4096 + 256 * len), including context ancestry
 * and result copying. Depth uses doc's limit from the detached fragment root;
 * insertion rechecks depth from the destination tree. Temporary parsing and
 * final copying both charge doc's remaining arena, so peak storage includes
 * both until commit. Status is OK, BAD_ARGUMENT, NO_MEMORY, ARENA_EXHAUSTED,
 * TOO_DEEP or WORK_EXHAUSTED. On failure all doc accounting and state remain
 * unchanged; on success only allocation counts/peak and owned storage change.
 * Tree version, parse refusal/work/errors, landmarks and pins do not change.
 * Calls on one document must be serialized with its other operations. */
os64_html_node_t *os64_html_parse_fragment(os64_html_document_t *doc,
                                           const os64_html_node_t *context,
                                           const char *utf8, size_t len,
                                           bool scripting, int64_t *status);
/* innerHTML when children_only, outerHTML otherwise; document and fragment
 * nodes have no wrapper. HTML template contents are traversed and HTML void
 * elements have no children/end tag in output. Text escapes &, NBSP, <, >
 * except in HTML raw-text parents (including noscript when scripting); attrs
 * also escape double quotes. Names retain namespace-qualified spelling.
 * Allocation-free. Returns total bytes excluding NUL (saturates at SIZE_MAX),
 * writes a byte prefix of at most cap-1 and NUL when out != NULL and cap > 0.
 * cap == 0 writes nothing; truncation may split UTF-8. NULL node is empty.
 * Do not run this concurrently with a mutation of the node's document. */
size_t os64_html_serialize(const os64_html_node_t *node, bool children_only,
                           bool scripting, char *out, size_t cap);

#pragma GCC visibility pop

#endif
