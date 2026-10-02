#ifndef OS64_HTML_H
#define OS64_HTML_H

#include "html/tags.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct os64_html_parser os64_html_parser_t;

typedef enum {
    OS64_HTML_OK = 0,
    OS64_HTML_TOO_LARGE = -1,
    OS64_HTML_ARENA_EXHAUSTED = -2,
    OS64_HTML_NO_MEMORY = -3,
    OS64_HTML_TOO_DEEP = -4,
    OS64_HTML_WORK_EXHAUSTED = -5,
    /* What a verb that changes a finished document answers (below). */
    OS64_HTML_HIERARCHY = -6,      /* the tree the DOM Standard refuses to make */
    OS64_HTML_NOT_FOUND = -7,      /* the reference child is not the parent's child */
    OS64_HTML_BAD_TEXT = -8,       /* not UTF-8, or it holds a NUL */
    OS64_HTML_ROOT_REQUIRED = -9,  /* the document would lose its html element */
    OS64_HTML_BAD_ARGUMENT = -10
} os64_html_status_t;

/* What the program exits with when a document is freed while a snapshot
 * still has it pinned, or a pin that is not held is released ("HTML"). */
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
 * Constructor failure returns NULL, including a budget below initial storage.
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

/* ── CHANGING A FINISHED DOCUMENT (docs/design/pending/DOM.md) ────────────
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
 * A NODE LIVES AS LONG AS ITS DOCUMENT. `remove` unlinks; nothing frees a
 * node before os64_html_document_free. What a verb allocates is charged to
 * the document's `max_arena_bytes`.
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
 * answers "has the tree moved since?". The library calls nobody.
 *
 * `os64_html_pin` answers a pin, or 0 when the document has none left to
 * give (it has sixteen). No two pins in a program are answered with the same
 * number, so a pin names one taking of it and nothing later. Freeing a
 * document while one is held, or releasing what is not held (a pin let go
 * already, or one taken on another document), ends the program
 * (OS64_HTML_FATAL_EXIT): the holder is about to read freed memory.
 * `os64_html_retired_bytes` is what the pins are keeping alive right now.
 *
 * EVERY NODE A VERB IS HANDED BELONGS TO `doc`, the source of a clone
 * excepted. A node lies in its document's arena and goes when that document
 * is freed, so one document's node in another's tree would be a pointer to
 * freed memory in waiting; a verb refuses it (OS64_HTML_BAD_ARGUMENT). */
typedef uint64_t os64_html_pin_t;
uint64_t os64_html_version(const os64_html_document_t *doc);
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
 * is left empty (inserting an empty one changes nothing, and the version
 * does not move). `replace` puts `node` where `old` is. `remove` unlinks a
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
int64_t os64_html_set_attr(os64_html_document_t *doc, os64_html_node_t *element,
                           const char *name, const char *value, size_t value_len);
int64_t os64_html_remove_attr(os64_html_document_t *doc, os64_html_node_t *element,
                              const char *name);
int64_t os64_html_set_text(os64_html_document_t *doc, os64_html_node_t *node, const char *utf8,
                           size_t len);

#pragma GCC visibility pop

#endif
