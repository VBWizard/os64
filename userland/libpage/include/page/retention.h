#ifndef OS64_PAGE_RETENTION_H
#define OS64_PAGE_RETENTION_H
#include "page/page.h"

#define OS64_PAGE_RETENTION_SCAN_MAX ((size_t)64)
enum {
    OS64_PAGE_RETENTION_UNKNOWN_INCOMING = 1u << 0,
    OS64_PAGE_RETENTION_UNKNOWN_EXPORTS = 1u << 1,
    OS64_PAGE_RETENTION_UNKNOWN_GROUP_BORROWS = 1u << 2,
    OS64_PAGE_RETENTION_UNKNOWN_RETIRED_BYTES = 1u << 3,
    OS64_PAGE_RETENTION_UNKNOWN_MODEL_REFERENCES = 1u << 4,
    OS64_PAGE_RETENTION_UNKNOWN_MODELS = 1u << 5
};
typedef struct {
    bool valid; /* non-NULL live state; descriptive, not an eligibility check */
    unsigned unknown;
    size_t physical_bytes, models_seen;
    bool models_complete, grouped, private_owner;
} os64_page_retention_t;

/* Owner-thread snapshot over a live state, without allocation, collection,
 * callbacks, mutation or lifetime extension. Visits at most
 * min(scan_limit, SCAN_MAX) distinct model objects, not model references.
 * When incomplete, models_seen is a lower bound, including when zero.
 * Physical charges stay with their allocating state across adoption and
 * include retired shared values; the retired subset is not enumerated.
 * Other group states can have models retaining this state's value bytes.
 * No count certifies release. Keep models/state/HTML alive under their
 * existing contracts; NULL returns an invalid, unknown snapshot. */
#pragma GCC visibility push(default)
os64_page_retention_t os64_page_retention_snapshot(const os64_page_state_t *, size_t scan_limit);
#pragma GCC visibility pop
#endif
