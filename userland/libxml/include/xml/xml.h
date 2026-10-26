#ifndef OS64_XML_H
#define OS64_XML_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__)
#define XML_API __attribute__((visibility("default")))
#else
#define XML_API
#endif

typedef struct xml_parser xml_parser_t;
typedef struct {
    const char *data;
    size_t size;
} xml_slice_t;
typedef struct {
    size_t byte, line, column;
} xml_location_t;
typedef struct {
    xml_slice_t uri, local, prefix;
} xml_name_t;
typedef struct {
    xml_name_t name;
    xml_slice_t value;
} xml_attribute_t;
typedef struct {
    xml_slice_t prefix, uri;
} xml_namespace_t;
typedef enum {
    XML_NEED_INPUT,
    XML_DONE,
    XML_MALFORMED,
    XML_UNSUPPORTED,
    XML_UNSUPPORTED_ENCODING,
    XML_LIMIT,
    XML_NO_MEMORY,
    XML_CANCELLED,
    XML_CONSUMER_ABORT,
    XML_BAD_ARGUMENT
} xml_status_t;
typedef enum {
    XML_START_DOCUMENT,
    XML_END_DOCUMENT,
    XML_DECLARATION,
    XML_START_ELEMENT,
    XML_END_ELEMENT,
    XML_TEXT,
    XML_CDATA,
    XML_COMMENT,
    XML_PI,
    XML_DOCTYPE
} xml_event_kind_t;
typedef struct {
    xml_event_kind_t kind;
    xml_location_t location;
    bool in_doctype; /* Internal-subset comment/PI; not a document child. */
    xml_name_t name;
    xml_slice_t text, target, encoding, version, public_id, system_id;
    int standalone; /* -1 absent, 0 no, 1 yes; declaration events only. */
    const xml_attribute_t *attributes;
    size_t attribute_count;
    const xml_namespace_t *namespaces;
    size_t namespace_count;
} xml_event_t;
typedef struct {
    size_t encoded_bytes, decoded_bytes, expanded_bytes, live_bytes, work;
    size_t depth, token_bytes, name_bytes, attributes, namespaces, entities;
} xml_limits_t;
typedef struct {
    size_t encoded_bytes, decoded_bytes, expanded_bytes, live_bytes, peak_bytes;
    size_t work, depth, peak_depth, events;
} xml_stats_t;
/* allocate returns max_align_t-aligned storage. release receives the exact
   requested size. Contexts remain alive until xml_destroy returns. */
typedef struct {
    void *context;
    void *(*allocate)(void *context, size_t size);
    void (*release)(void *context, void *memory, size_t size);
} xml_allocator_t;
typedef struct {
    xml_allocator_t memory;
    void *context;
    /* Slices and arrays are borrowed until return. false aborts the parse.
       Do not feed, finish or destroy this parser from either callback. */
    bool (*event)(void *context, const xml_event_t *event);
    bool (*cancelled)(void *context);
    xml_limits_t limits; /* Each zero field selects that field's default. */
} xml_options_t;

XML_API xml_limits_t xml_default_limits(void);
XML_API xml_status_t xml_create(const xml_options_t *options, xml_parser_t **out);
/* consumed counts bytes taken from this call, including a consumed byte that
   exposed malformed syntax. Limits/cancellation can stop before the next byte.
   Input need not survive this call. Call finish after final input. */
XML_API xml_status_t xml_feed(xml_parser_t *, const void *, size_t, size_t *consumed);
XML_API xml_status_t xml_finish(xml_parser_t *);
XML_API void xml_destroy(xml_parser_t *);
XML_API xml_status_t xml_status(const xml_parser_t *);
XML_API xml_stats_t xml_stats(const xml_parser_t *);
/* Error location identifies the current token/decoder scalar. */
XML_API xml_location_t xml_error_location(const xml_parser_t *);
/* A borrowed label, retained after UNSUPPORTED_ENCODING until destroy. */
XML_API xml_slice_t xml_encoding(const xml_parser_t *);
XML_API const char *xml_status_name(xml_status_t);
#endif
