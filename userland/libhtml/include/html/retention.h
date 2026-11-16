#ifndef OS64_HTML_RETENTION_H
#define OS64_HTML_RETENTION_H
#include "html/html.h"

#define OS64_HTML_RETENTION_SCAN_MAX ((size_t)64)
enum {
    OS64_HTML_RETENTION_UNKNOWN_INCOMING = 1u << 0,
    OS64_HTML_RETENTION_UNKNOWN_EXPORTS = 1u << 1,
    OS64_HTML_RETENTION_UNKNOWN_GROUP_BORROWS = 1u << 2,
    OS64_HTML_RETENTION_UNKNOWN_PENDING_NODES = 1u << 3,
    OS64_HTML_RETENTION_UNKNOWN_READERS = 1u << 4,
    OS64_HTML_RETENTION_UNKNOWN_RANGES = 1u << 5
};
typedef struct {
    size_t seen;
    bool complete; /* otherwise seen is a lower bound, including when zero */
} os64_html_retention_count_t;
typedef struct {
    os64_html_status_t status;
    unsigned unknown;
    uint32_t arena_id; /* diagnostic identity, not a recyclable slot token */
    uint8_t creation_realm;
    size_t arena_bytes, peak_arena_bytes, retired_block_bytes;
    size_t holds, snapshot_pins;
    os64_html_retention_count_t readers, associated_ranges;
    bool grouped, charged_to_group, parser_active;
} os64_html_retention_t;
typedef struct {
    os64_html_retention_t owner; /* physical allocation owner after adoption */
    uint32_t logical_document_arena_id;
    uint32_t node_nonparser_refs;
    uint16_t parser_refs;
    uint8_t creation_realm;
} os64_html_node_retention_t;

/* Owner-thread queries over live caller-owned inputs, returned by value.
 * No allocation, collection, callbacks, mutation or lifetime extension.
 * Each list visits at most min(scan_limit, SCAN_MAX) entries; completeness
 * describes that local list, not incoming edges or other group members.
 * Charges and holds belong to the physical arena. Ranges are associated
 * with a node document and may migrate without moving their allocations.
 * retired_block_bytes excludes pending detached node units, whose census
 * would require an unbounded tree walk. A sibling's pin can retain bytes
 * here even when snapshot_pins is zero. OK/zero never certifies release.
 * NULL answers BAD_ARGUMENT with unknown coverage. Existing holds/pins and
 * group teardown contracts still govern input lifetime. node_nonparser_refs
 * includes internal log references and need not equal owner.holds. */
#pragma GCC visibility push(default)
os64_html_retention_t os64_html_retention_snapshot(const os64_html_document_t *, size_t scan_limit);
os64_html_node_retention_t os64_html_node_retention_snapshot(const os64_html_node_t *, size_t scan_limit);
#pragma GCC visibility pop
#endif
