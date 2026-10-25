#ifndef XML_INTERNAL_H
#define XML_INTERNAL_H
#include "include/xml/xml.h"
typedef struct {
    char *data;
    size_t size, capacity;
} buffer;
typedef struct binding {
    struct binding *next;
    xml_slice_t prefix, uri;
} binding;
typedef struct frame {
    struct frame *parent;
    binding *scope;
    xml_slice_t qname;
    xml_name_t name;
} frame;
typedef struct entity {
    struct entity *next;
    xml_slice_t name, value;
} entity;
struct xml_parser {
    xml_options_t options;
    xml_status_t status;
    xml_stats_t stats;
    xml_location_t location, token_location, error_location;
    buffer token, label;
    frame *top;
    binding *bindings;
    entity *entities;
    size_t binding_count, entity_count;
    unsigned char probe[4];
    unsigned probe_count, encoding, pending, needed;
    uint32_t scalar, minimum, high_surrogate;
    size_t scalar_byte, dtd_comment_end;
    unsigned quote, subset, dtd_mode;
    bool detected, bom, cr, markup, busy, started, root_seen, root_closed;
    bool declaration_allowed, doctype_seen, external_subset, standalone, in_doctype;
};
#endif
