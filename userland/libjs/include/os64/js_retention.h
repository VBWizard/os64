#ifndef OS64_JS_RETENTION_H
#define OS64_JS_RETENTION_H
#include "os64/js.h"

/* Families not enumerated by the scalar wrapper snapshot. A zero counter
 * does not establish absence of an edge from any of these families. */
enum {
    OS64_JS_RETENTION_UNKNOWN_ENGINE_GRAPH = 1u << 0,
    OS64_JS_RETENTION_UNKNOWN_JOB_ROLES = 1u << 1,
    OS64_JS_RETENTION_UNKNOWN_CALLBACK_CONTEXTS = 1u << 2,
    OS64_JS_RETENTION_UNKNOWN_RAW_EXPORTS = 1u << 3,
    OS64_JS_RETENTION_UNKNOWN_NATIVE_OWNERS = 1u << 4,
    OS64_JS_RETENTION_UNKNOWN_EXPOSURE = 1u << 5,
    OS64_JS_RETENTION_UNKNOWN_ALL = (1u << 6) - 1
};
typedef struct {
    os64_js_status_t status;
    unsigned unknown;
    size_t occupied_slots, context_roots, promise_then_roots;
    size_t retired_slots, failed_slots, staged_slots;
    size_t module_owner_slots; /* slots with wrapper module handles, not module count */
    size_t rejection_records; /* the wrapper's unhandled-rejection ledger */
    size_t heap_bytes, heap_peak; /* runtime-wide allocator charges, not per-realm bytes */
    os64_js_status_t latched_status;
    os64_js_limit_t latched_limit;
    unsigned allocator_failures; /* opaque port flags: nonzero means allocator failure */
    bool jobs_pending, active, task_open, turn_open;
    bool setup_active, settings_active;
    bool cancelled, destroying;
} os64_js_retention_t;

/* Owner thread, live runtime only. O(OS64_JS_MAX_REALMS), no allocation,
 * script, GC, clock or failure observation. Usable inside native callbacks
 * and after sticky failure; destruction refuses before touching engine state.
 * Status OK describes the snapshot, not runtime health or safe reclamation.
 * Latched health is copied without polling: cancellation/allocator facts can
 * therefore be newer than the latch. Counts describe occupied wrapper slots;
 * private setup in progress is also indicated by setup_active.
 * Returned by value so no borrowed outcome/output storage can be overwritten.
 * This neither supplies a reachability certificate nor changes admission. */
os64_js_retention_t os64_js_retention_snapshot(const os64_js_runtime_t *, const char *caller_abi);
#endif
