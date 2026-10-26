#ifndef OS64_HTML_XML_H
#define OS64_HTML_XML_H
#include "html/html.h"
#include "xml/xml.h"

/* Native XML-to-shared-tree adapter; no browser or script context required. */
typedef struct os64_html_xml_parser os64_html_xml_parser_t;
typedef struct {
    size_t max_arena_bytes; /* Shared parser/scratch/tree ledger, including headers. */
    uint64_t max_tree_work;
    xml_limits_t parser_limits; /* Zero fields use libxml defaults. */
    void *context;
    bool (*cancelled)(void *context);
} os64_html_xml_options_t;
typedef struct {
    xml_status_t status;
    int64_t tree_status;
    xml_location_t location;
    xml_stats_t parser;
    uint64_t tree_work;
    /* Snapshot before finish releases parser/scratch; doc retains its own ledger. */
    size_t arena_bytes, peak_arena_bytes;
} os64_html_xml_result_t;

#pragma GCC visibility push(default)
os64_html_xml_options_t os64_html_xml_options_default(void);
/* NULL options select defaults; the two tree limits are literal when supplied.
   Calls on one handle are serialized. Do not reenter from cancelled(). */
os64_html_xml_parser_t *os64_html_xml_parser_new(const os64_html_xml_options_t *options,
                                                 os64_html_xml_result_t *result);
xml_status_t os64_html_xml_parser_feed(os64_html_xml_parser_t *parser, const void *bytes,
                                       size_t length, size_t *consumed);
os64_html_xml_result_t os64_html_xml_parser_result(const os64_html_xml_parser_t *parser);
/* Borrowed until finish/destroy; useful after UNSUPPORTED_ENCODING. */
xml_slice_t os64_html_xml_parser_encoding(const os64_html_xml_parser_t *parser);
/* Consumes the handle. Returns an independently owned document only on DONE;
   otherwise frees the partial tree. Use os64_html_document_free for success.
   Adjacent TEXT/CDATA events coalesce to TEXT; PI and comments stay distinct.
   HTML landmarks are NULL; use os64_html_document_element. */
os64_html_document_t *os64_html_xml_parser_finish(os64_html_xml_parser_t *parser,
                                                  os64_html_xml_result_t *result);
void os64_html_xml_parser_destroy(os64_html_xml_parser_t *parser);
#pragma GCC visibility pop
#endif
