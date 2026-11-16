#ifndef OS64_DOM_KEPT_RETENTION_H
#define OS64_DOM_KEPT_RETENTION_H
#include "dom/dom.h"

#define OS64_DOM_KEPT_RETENTION_SCAN_MAX ((size_t)64)
enum {
    OS64_DOM_KEPT_UNKNOWN_INCOMING = 1u << 0,
    OS64_DOM_KEPT_UNKNOWN_REFERENCE_ROLES = 1u << 1,
    OS64_DOM_KEPT_UNKNOWN_OTHER_PAYLOADS = 1u << 2,
    OS64_DOM_KEPT_UNKNOWN_ALLOCATION_OVERHEAD = 1u << 3,
    OS64_DOM_KEPT_UNKNOWN_PIECE_DETAIL = 1u << 4,
    OS64_DOM_KEPT_UNKNOWN_RECORDS = 1u << 5,
    OS64_DOM_KEPT_UNKNOWN_ALL = (1u << 6) - 1
};
typedef struct {
    os64_js_status_t status;
    unsigned unknown;
    size_t quota_bytes, records_seen, quota_bytes_seen;
    size_t shared_records, empty_records, segmented_records, block_records;
    size_t max_record_references;
    bool records_complete, closed;
} os64_dom_kept_retention_t;

/* Owner-thread, by-value observation of one live binding's kept-body ledger.
 * No engine entry, allocation, callbacks, collection, clock or reference
 * release. Valid after drain/engine destruction, before binding free.
 * Visits at most min(scan_limit, SCAN_MAX) records, without walking pieces.
 * Prefix counts/charges are lower bounds until records_complete is true;
 * quota_bytes is the binding's existing full quota ledger. Quota includes
 * body/piece storage and Blob records, but excludes response records and
 * allocator overhead: zero quota does not imply no allocated records.
 * shared_records counts records whose native reference counter exceeds one;
 * max_record_references is a prefix maximum, not a count of script aliases
 * or a classification of their owners. Other payload families and incoming
 * edges remain unknown. OK/zero never certifies release or slot reuse.
 * NULL arguments or an ABI mismatch refuse data without inspecting records. */
#pragma GCC visibility push(default)
os64_dom_kept_retention_t os64_dom_kept_retention_snapshot(const os64_dom_t *,
    const char *caller_abi, size_t scan_limit);
#pragma GCC visibility pop
#endif
