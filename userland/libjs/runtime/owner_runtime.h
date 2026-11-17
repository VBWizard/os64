#ifndef JS_OWNER_RUNTIME_H
#define JS_OWNER_RUNTIME_H
#include "owner_incarnation.h"
#include "os64/js_engine.h"

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
    size_t assigned_owners, unpublished_owners;
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
typedef struct {
    JSOwnerId owner;
    bool primary, staged, retired, failed;
} JSOwnerRealmSnapshot;
/* Private checked native operations. Ticket/snapshot outputs are distinct
 * caller storage outside runtime/metadata and execution outcomes, unchanged
 * on refusal. Execution outcomes follow the existing ABI 5 call rules. IDs refer
 * to the original retained realm, not an F4 context/target or origin.
 * Issuing/duplicating refuses destruction; close and scalar inspection can
 * participate in synchronous finalizer cleanup while the runtime is live. */
JSOwnerStatus js_owner_runtime_primary_ticket(os64_js_runtime_t *, JSOwnerTicket *);
/* A known raw realm is a compatibility path: its valid conversion records
 * sticky unknown retention, preserving the raw handle's ABI 5 lifetime. */
JSOwnerStatus js_owner_runtime_realm_ticket(os64_js_runtime_t *, const os64_js_realm_t *, JSOwnerTicket *);
JSOwnerStatus js_owner_runtime_duplicate(os64_js_runtime_t *, JSOwnerTicket, JSOwnerTicket *);
JSOwnerStatus js_owner_runtime_close(os64_js_runtime_t *, JSOwnerTicket);
JSOwnerStatus js_owner_runtime_owner(const os64_js_runtime_t *, JSOwnerTicket, JSOwnerRealmSnapshot *);
os64_js_status_t js_owner_runtime_child(os64_js_runtime_t *, const char *caller_abi,
    size_t required_headroom, JSOwnerTicket *out, os64_js_outcome_t *);
/* Run uses existing source/run admission and settings; its independent loan
 * spans engine/value cleanup and draining. No raw view or persistent value
 * is returned. Retirement uses the existing F3 transition. Neither API
 * creates a security, target-publication or reclamation certificate. */
os64_js_status_t js_owner_runtime_run(os64_js_runtime_t *, const char *caller_abi,
    JSOwnerTicket, const void *source, size_t length, const char *name, os64_js_outcome_t *);
os64_js_status_t js_owner_runtime_retire(os64_js_runtime_t *, const char *caller_abi,
    JSOwnerTicket, os64_js_outcome_t *);
/* This private callback lends a context and temporary values through the
 * admitted invocation and its synchronous cleanup. The opaque scope/view
 * must not outlive that invocation. Return one owned value (or JS_EXCEPTION)
 * for wrapper cleanup; retain no context/value/native borrower unless an
 * explicit compatibility export precedes it. This is no persistent root API. */
typedef struct JSOwnerRuntimeScope JSOwnerRuntimeScope;
typedef JSValue JSOwnerScopedBody(JSOwnerRuntimeScope *, JSContext *, void *);
os64_js_status_t js_owner_runtime_scoped_run(os64_js_runtime_t *, const char *caller_abi,
    JSOwnerTicket, const char *name, JSOwnerScopedBody *, void *, os64_js_outcome_t *);
/* Live-scope metadata operations. Promotion creates an independent ticket
 * even after the admitting holder closes. Capacity/exhaustion leaves output
 * and loan unchanged. Outputs are distinct caller storage outside the scope.
 * Export uses ABI 5 getter admission and records sticky compatibility retention
 * before publishing its runtime-lived context; it does not clear other gates. */
JSOwnerStatus js_owner_scope_owner(const JSOwnerRuntimeScope *, JSOwnerId *);
JSOwnerStatus js_owner_scope_promote(JSOwnerRuntimeScope *, JSOwnerTicket *);
JSOwnerStatus js_owner_scope_export(JSOwnerRuntimeScope *, JSContext **);
#endif
