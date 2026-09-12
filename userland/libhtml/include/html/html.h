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
    OS64_HTML_WORK_EXHAUSTED = -5
} os64_html_status_t;

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

/* Nodes and attributes are read-only views owned by their document. */
typedef struct os64_html_node {
    os64_html_node_kind_t kind;
    os64_html_ns_t ns;
    os64_html_tag_t tag;
    const char *name;
    const char *public_id, *system_id;
    const char *text;
    size_t text_len;
    os64_html_attr_t *attrs;
    struct os64_html_node *template_contents;
    struct os64_html_node *parent, *first_child, *last_child, *prev, *next;
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

#pragma GCC visibility pop

#endif
