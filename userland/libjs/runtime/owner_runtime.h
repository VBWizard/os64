#ifndef JS_OWNER_RUNTIME_H
#define JS_OWNER_RUNTIME_H
#include "owner_incarnation.h"
#include "os64/js_engine.h"
#include "os64/js_callbacks.h"
#include "os64/js_failure.h"

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
typedef struct { uint64_t incarnation, serial; } JSOwnerContextId;
/* Private scalar identity, not a holder. Read original retained realm facts
 * without allocation, raw export or health polling. Context IDs come from a
 * separate monotonic namespace in the charged runtime arena. No wrap/reuse;
 * admission may burn IDs. Both outputs remain unchanged on refusal. */
JSOwnerStatus js_owner_runtime_owner_snapshot(const os64_js_runtime_t *, JSOwnerId, JSOwnerRealmSnapshot *);
JSOwnerStatus js_owner_runtime_context_id(os64_js_runtime_t *, JSOwnerContextId *);
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
 * for wrapper cleanup; retain no context/value/native borrower in untracked
 * storage unless an explicit compatibility export precedes it. An explicit
 * checked capture owns its separately bounded C root; the scope and context
 * remain invocation-only views. */
typedef struct JSOwnerRuntimeScope JSOwnerRuntimeScope;
typedef JSValue JSOwnerScopedBody(JSOwnerRuntimeScope *, JSContext *, void *);
os64_js_status_t js_owner_runtime_scoped_run(os64_js_runtime_t *, const char *caller_abi,
    JSOwnerTicket, const char *name, JSOwnerScopedBody *, void *, os64_js_outcome_t *);
/* Explicit settings invocation borrowing an active guard or open task. Entry
 * and a supplied incumbent each reserve an independent loan, including when
 * their tickets name the same owner. NULL incumbent supplies no replacement
 * backup; incumbent selection follows the existing engine settings rules.
 * JSOwnerScopedBody's invocation-only contract applies; result/exception
 * diagnostics and value cleanup finish inside the selected settings frame.
 * It returns no value and selects no exception-reporting hook. It runs no
 * checkpoint and preserves the task budget. The caller supplies original
 * eligible realms, not F4 authority.
 * Existing setup/failure/lifecycle and outcome-alias refusal rules apply. */
os64_js_status_t js_owner_runtime_with_settings(os64_js_runtime_t *, const char *caller_abi,
    JSOwnerTicket entry, const JSOwnerTicket *incumbent, JSOwnerScopedBody *, void *, os64_js_outcome_t *);
typedef struct { uint64_t incarnation, serial; } JSOwnerCallbackKey;
typedef struct {
    uint64_t incarnation;
    size_t capacity, live, closing, invocations;
} JSOwnerCallbackSummary;
/* Private C-root family selected before publication. The finite record pool
 * joins the charged owner arena; other constructors select no callback cells.
 * Keys alias one record and contain no native pointer. They do not keep the
 * supplied runtime alive, establish an associated entry or grant authority. */
os64_js_status_t js_owner_runtime_create_with_callbacks(const os64_js_config_t *,
    os64_js_teardown_policy_t, const char *caller_abi, JSOwnerLimits, size_t callback_capacity,
    os64_js_runtime_t **out, os64_js_outcome_t *);
/* Numeric owner-thread inspection, including live finalizer unwind. Refusal
 * leaves output unchanged. Zero counts are not a graph/release certificate. */
bool js_owner_callback_summary(const os64_js_runtime_t *, JSOwnerCallbackSummary *);
/* Caller validates its callback type and lends a live same-engine object.
 * Capture owns that C root and an independent actual-incumbent holder. It
 * allocates/polls nothing and preserves exception/output on refusal. Scope
 * remains invocation-only; numeric output uses distinct caller storage. */
JSOwnerStatus js_owner_callback_capture(JSOwnerRuntimeScope *, const char *caller_abi,
    JSValueConst callback, JSOwnerCallbackKey *out);
/* No-result invocation borrows callback/context/scope through synchronous
 * cleanup. The host separately retains and validates associated entry.
 * Body/export rules match JSOwnerScopedBody. Closing/reusing the key during
 * reentry leaves the admitted value and original role loans alive. */
typedef JSValue JSOwnerCallbackBody(JSOwnerRuntimeScope *, JSContext *, JSValueConst, void *);
os64_js_status_t js_owner_callback_invoke(os64_js_runtime_t *, const char *caller_abi,
    JSOwnerCallbackKey, JSOwnerTicket entry, JSOwnerCallbackBody *, void *, os64_js_outcome_t *);
/* Invalidate before value release. Clear/drain keeps incumbent and cell
 * storage through native finalizers. Its bookkeeping reserves no new loans,
 * allocates no cleanup records and polls no clock; value finalizers still run.
 * Nested closes queue bounded cleanup; the outermost cleanup drains it.
 * New capture/invocation refuses during cleanup. Stale aliases cannot close
 * a replacement. Root shutdown drains owned records before contexts/engine. */
JSOwnerStatus js_owner_callback_close(os64_js_runtime_t *, const char *caller_abi, JSOwnerCallbackKey);
/* Live-scope metadata operations. Promotion creates an independent ticket
 * even after the admitting holder closes. Capacity/exhaustion leaves output
 * and loan unchanged. Outputs are distinct caller storage outside the scope.
 * Export uses ABI 5 getter admission and records sticky compatibility retention
 * before publishing its runtime-lived context; it does not clear other gates. */
JSOwnerStatus js_owner_scope_owner(const JSOwnerRuntimeScope *, JSOwnerId *);
typedef enum {
    JS_OWNER_SETTINGS_ABSENT, JS_OWNER_SETTINGS_MAPPED, JS_OWNER_SETTINGS_UNMAPPED
} JSOwnerSettingsKind;
typedef struct {
    JSOwnerId admitting_owner;
    JSOwnerRealmSnapshot realm;
    JSOwnerSettingsKind kind;
} JSOwnerSettingsSnapshot;
/* Live-scope scalar observation under an active wrapper settings frame. The
 * admitting owner is independent of the selected role's original realm.
 * Non-mapped realm fields are zero. No allocation, health polling, raw view,
 * holder or authority is supplied; lifecycle facts do not grant eligibility.
 * Output is distinct caller storage outside runtime/scope/active outcomes;
 * refusal leaves it unchanged. Copied facts survive scope/root teardown. */
JSOwnerStatus js_owner_scope_settings(const JSOwnerRuntimeScope *,
    os64_js_settings_role_t, JSOwnerSettingsSnapshot *);
JSOwnerStatus js_owner_scope_promote(JSOwnerRuntimeScope *, JSOwnerTicket *);
JSOwnerStatus js_owner_scope_export(JSOwnerRuntimeScope *, JSContext **);
/* Explicit private diagnostic selection before runtime publication. This
 * composes charged owner admission with existing terminal failure recording;
 * no recovery/reset or public checked profile is enabled. */
os64_js_status_t js_owner_runtime_create_with_failure_records(const os64_js_config_t *,
    os64_js_teardown_policy_t, const char *caller_abi, JSOwnerLimits,
    os64_js_runtime_t **out, os64_js_outcome_t *);
typedef enum {
    JS_OWNER_FAILURE_NONE, JS_OWNER_FAILURE_ASSIGNED,
    JS_OWNER_FAILURE_UNPUBLISHED, JS_OWNER_FAILURE_UNMAPPED
} JSOwnerFailureRefKind;
typedef struct {
    JSOwnerId owner;
    JSOwnerFailureRefKind kind;
} JSOwnerFailureRef;
typedef struct {
    uint64_t incarnation;
    os64_js_failure_cause_t original, global;
    int64_t deadline;
    uint64_t jobs_executed, jobs_discarded;
    JSOwnerFailureRef task_owner, realms[OS64_JS_MAX_REALMS];
    size_t realm_count;
    unsigned scope_issues;
    bool turn_open, explicit_task, scopes_classified, owners_mapped;
    JSOwnerFailureRef job_execution, job_callback, job_activity;
} JSOwnerFailureSnapshot;
/* Owner-thread, live-runtime observation. Output is distinct caller storage
 * outside runtime/metadata/active outcomes; refusal leaves it unchanged and
 * polls no health. Successful observation follows existing health precedence,
 * without exporting pointers, acquiring holders or changing legacy retention.
 * Copied scalar facts survive close/destruction. Mapping completeness is
 * separate from engine scope classification; neither grants recovery/release.
 * Unmapped/absent references carry zero IDs, not reusable address keys. */
os64_js_status_t js_owner_runtime_observe_failure(os64_js_runtime_t *,
    const char *caller_abi, JSOwnerFailureSnapshot *);
/* Trusted native bootstrap with the same invocation-only borrowed scope as
 * scoped_run. Free temporary values before returning. Return OK or
 * ADMISSION_REFUSED without an exception, or EXCEPTION with its pending engine
 * exception for wrapper diagnostics. No persistent raw view or
 * borrower escapes unless explicit compatibility export precedes it. */
typedef os64_js_status_t JSOwnerSetupBody(JSOwnerRuntimeScope *, JSContext *, void *);
os64_js_status_t js_owner_runtime_stage(os64_js_runtime_t *, const char *caller_abi,
    size_t required_headroom, JSOwnerTicket *out, os64_js_outcome_t *);
/* Metadata refusal precedes installation and preserves the staged candidate.
 * An admitted loan spans installation, candidate jobs/rejections, transaction
 * rollback and setup unwind, including close of the admitting ticket. Setup
 * success alone does not activate the realm. Neither operation publishes an
 * F4 target or grants authority, root release or slot reuse. */
os64_js_status_t js_owner_runtime_setup(os64_js_runtime_t *, const char *caller_abi,
    JSOwnerTicket, JSOwnerSetupBody *, void *, os64_js_outcome_t *);
os64_js_status_t js_owner_runtime_activate(os64_js_runtime_t *, const char *caller_abi,
    JSOwnerTicket, os64_js_outcome_t *);
/* Status-only registration preserves the legacy host/opaque lifetime and
 * detach-after-failure rules. Hold a ticket until successful detach or root
 * shutdown. Closing a holder does not detach the host. Legacy host callbacks
 * retain their raw ABI 5 views and record compatibility retention on delivery. */
os64_js_status_t js_owner_runtime_set_script_host(os64_js_runtime_t *, const char *caller_abi,
    JSOwnerTicket, const os64_js_script_host_t *, os64_js_outcome_t *);
/* Borrow promise/reason/tag for one invocation and synchronous cleanup. Return
 * one owned value or JS_EXCEPTION, as for scoped_run; no persistent value or
 * native borrower may escape without explicit compatibility export. The loan
 * spans fresh-turn draining; an existing explicit task retains its budget and
 * queued work for its own checkpoint/end. Pending microtasks refuse admission.
 * consumed is distinct caller storage outside runtime/scope/outcome. Refusal
 * before claim preserves it and the record; healthy empty polling sets false
 * without opening a turn or loan. Claim sets true even if later work fails.
 * Queue roots are released under guard/loan after callback/value diagnostics.
 * This consumes queue ownership, not an owner/provider reclamation proof. */
typedef JSValue JSOwnerRejectionBody(JSOwnerRuntimeScope *, JSContext *,
    JSValueConst promise, JSValueConst reason, uint32_t tag, void *);
os64_js_status_t js_owner_runtime_consume_rejection(os64_js_runtime_t *, const char *caller_abi,
    JSOwnerTicket, const char *name, JSOwnerRejectionBody *, void *, bool *consumed, os64_js_outcome_t *);
/* Own one idle native task and checkpoint its ordinary jobs to exhaustion,
 * preserving the first exception diagnostic. The independent loan spans
 * body/result/job/rejection cleanup and task/guard closure. Sticky failure
 * closes the task without resetting its verdict or budget. Open-task/nested
 * use refuses; this is not a persistent task handle or deferred-task scope.
 * JSOwnerScopedBody's borrowing/export contract applies throughout cleanup.
 * No F4 authority, public activation or reclamation certificate is created. */
os64_js_status_t js_owner_runtime_task_run(os64_js_runtime_t *, const char *caller_abi,
    JSOwnerTicket, const char *name, JSOwnerScopedBody *, void *, os64_js_outcome_t *);
/* Status-only module-provider registration/detachment. Keep native opaque until
 * detach/root shutdown, and returned source bytes until the enclosing call ends.
 * Retain a holder for cleanup: closing it does not detach a provider. Existing
 * private-setup/active-call/failure/retirement rules apply. Loader callbacks
 * keep their string/borrowed-byte contracts; legacy import-base callback
 * delivery retains its raw-view compatibility veto. No persistent module
 * handle, provider reclamation or F4 authority is supplied. */
os64_js_status_t js_owner_runtime_set_module_loader(os64_js_runtime_t *, const char *caller_abi,
    JSOwnerTicket, const os64_js_module_loader_t *, os64_js_outcome_t *);
os64_js_status_t js_owner_runtime_set_import_base(os64_js_runtime_t *, const char *caller_abi,
    JSOwnerTicket, os64_js_import_base_t *, void *, os64_js_outcome_t *);
#endif
