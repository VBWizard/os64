#ifndef OS64_DOM_RETENTION_H
#define OS64_DOM_RETENTION_H
#include "dom/dom.h"

enum {
    OS64_DOM_RETENTION_UNKNOWN_ENGINE_GRAPH = 1u << 0,
    OS64_DOM_RETENTION_UNKNOWN_CALLBACK_CONTEXTS = 1u << 1,
    OS64_DOM_RETENTION_UNKNOWN_RAW_EXPORTS = 1u << 2,
    OS64_DOM_RETENTION_UNKNOWN_HTML_BORROWS = 1u << 3,
    OS64_DOM_RETENTION_UNKNOWN_PAGE_BORROWS = 1u << 4,
    OS64_DOM_RETENTION_UNKNOWN_EXPOSURE = 1u << 5,
    OS64_DOM_RETENTION_UNKNOWN_NATIVE_INCOMING = 1u << 6,
    OS64_DOM_RETENTION_UNKNOWN_ALL = (1u << 7) - 1
};
typedef struct {
    os64_js_status_t status;
    unsigned unknown;
    size_t occupied_members, closed_members, installing_members, departure_finished_members;
    size_t retained_value_records, reported_registry_values;
    size_t html_bytes, page_bytes, dom_bytes, kept_bytes, canvas_bytes;
    size_t provider_borrows, dispatch_depth, activity_busy_members, layout_borrowers;
    uint64_t oldest_layout_generation; /* UINT64_MAX without a borrowed generation */
    uint64_t recovery_generation; /* F16 episode identity, not a slot-reuse token */
    bool drained, poisoned, recovering, recovery_running, recovery_completed, streams_used;
} os64_dom_retention_t;

/* Owner-thread scalar snapshot, bounded by the cluster's 65 members. No
 * engine entry, allocation, callback, collection, clock or loan acknowledgment.
 * Valid through drain and subsequent engine destruction, before cluster_free.
 * Counts reuse the existing native registry census: aliases can count in more
 * than one holder, and unclassified incoming edges remain explicitly unknown.
 * activity_busy_members is the departure guard, not a complete borrower census.
 * OK is descriptive; neither OK nor zero counters certifies reclamation.
 * Returned by value without modifying borrowed outcomes or provider state. */
#pragma GCC visibility push(default)
os64_dom_retention_t os64_dom_retention_snapshot(const os64_dom_cluster_t *, const char *caller_abi);
#pragma GCC visibility pop
#endif
