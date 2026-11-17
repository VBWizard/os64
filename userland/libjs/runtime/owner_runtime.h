#ifndef JS_OWNER_RUNTIME_H
#define JS_OWNER_RUNTIME_H
#include "owner_incarnation.h"

/* Private numeric inspection. The caller supplies a live runtime on its
 * owner thread, including synchronous finalizer unwind before destroy frees
 * the wrapper. NULL yields zero. A copied number keeps no storage alive and
 * grants no execution, target/security or reclamation authority. */
uint64_t js_owner_runtime_incarnation(const os64_js_runtime_t *);
typedef struct {
    uint64_t incarnation;
    JSOwnerLimits limits;
    JSOwnerSummary summary;
    size_t requested_bytes, charged_bytes, wrapper_bytes, engine_limit;
} JSOwnerRuntimeMetadata;
/* Explicit private construction alternative. Reserve metadata and its actual
 * host allocation charge before engine construction. Legacy constructors do
 * not select it. No public checked profile or exported owners are enabled. */
os64_js_status_t js_owner_runtime_create(const os64_js_config_t *, os64_js_teardown_policy_t,
    const char *caller_abi, JSOwnerLimits, os64_js_runtime_t **out, os64_js_outcome_t *);
/* Live-runtime/owner-thread scalar inspection, including finalizer unwind.
 * False (unselected/invalid) leaves output unchanged; output must be outside
 * runtime storage. Empty counts grant no lifetime or release authority. */
bool js_owner_runtime_metadata(const os64_js_runtime_t *, JSOwnerRuntimeMetadata *out);
#endif
