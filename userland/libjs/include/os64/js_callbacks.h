#ifndef OS64_JS_CALLBACKS_H
#define OS64_JS_CALLBACKS_H
#include "os64/js_engine.h"
#include "quickjs.h"

/* Native-owned callback reference and conversion-time incumbent. Initialize
 * with OS64_JS_CALLBACK_INIT; do not shallow-copy a populated record. Its
 * runtime must outlive the record. C roots clear before runtime destruction;
 * engine-owned holders trace with callback_mark and clear in their finalizer.
 * The holder accounts for this inline storage in its own memory budget. */
typedef struct {
    JSValue value;
    os64_js_runtime_t *runtime;
    os64_js_realm_t *incumbent;
} os64_js_callback_t;
#define OS64_JS_CALLBACK_INIT { JS_UNDEFINED, NULL, NULL }

/* Capture after the binding's callback-type validation, at conversion time.
 * Requires an object, an empty record and a checked incumbent scope. Does not
 * inspect properties, derive a function/entry realm, run script or allocate.
 * Success retains value; refusal leaves the record and exception untouched. */
os64_js_status_t os64_js_callback_capture(os64_js_runtime_t *runtime,
    const char *caller_abi, JSValueConst value, os64_js_callback_t *record,
    os64_js_outcome_t *outcome);
/* Clear before releasing the held value, including in a callback/finalizer
 * and after failure or retirement. Empty records are idempotent. No clock,
 * exception consumption or guard acquisition; normal value finalization runs. */
os64_js_status_t os64_js_callback_clear(os64_js_runtime_t *runtime,
    const char *caller_abi, os64_js_callback_t *record, os64_js_outcome_t *outcome);
/* Call from the owning engine object's gc_mark, for an engine-held record.
 * A C-root record must not be presented as an engine edge. */
void os64_js_callback_mark(JSRuntime *engine, const os64_js_callback_t *record,
    JS_MarkFunc *mark);

typedef JSValue os64_js_callback_body_t(JSContext *entry, JSValueConst callback, void *opaque);
/* The binding supplies the callback's separately established associated entry
 * realm and performs its invocation/conversions in body. This helper does not
 * infer that realm or implement a WebIDL callback type. The value and captured
 * incumbent are snapshotted for this invocation: body may clear/reuse record.
 * The held value is released before leaving the checked settings guard.
 * Return/result/exception rules are those of os64_js_realm_with_settings. */
os64_js_status_t os64_js_callback_with_settings(os64_js_realm_t *entry,
    const char *caller_abi, const os64_js_callback_t *record,
    os64_js_callback_body_t *body, void *opaque, JSValue *value,
    os64_js_outcome_t *outcome);
/* Enable checked ordinary host scopes for classic/module roots and realm_call.
 * Owner thread, before any task/evaluation. Jobs do not inherit these scopes:
 * Promise callback preparation requires its own capture/invocation protocol. */
os64_js_status_t os64_js_enable_host_settings(os64_js_runtime_t *runtime,
    const char *caller_abi, os64_js_outcome_t *outcome);

/* Enable Promise reaction/thenable capture and job entry in addition to host
 * settings, before execution. Captures retain engine contexts; queued jobs
 * check executable and captured-incumbent lifecycle independently. Native
 * bootstrap and neutral continuations retain their separate job contract.
 * Exact active ScriptOrModule restoration can supply an unskipped script
 * activation that takes precedence over the captured incumbent backup. */
os64_js_status_t os64_js_enable_promise_settings(os64_js_runtime_t *runtime,
    const char *caller_abi, os64_js_outcome_t *outcome);

/* Extends Promise settings to FinalizationRegistry construction and cleanup.
 * Retains captured incumbent and exact source record; registry activity and
 * executable eligibility remain independent. Uses the bounded engine queue,
 * not a browser task-source scheduler. Configuration precedes execution. */
os64_js_status_t os64_js_enable_finalization_settings(os64_js_runtime_t *runtime,
    const char *caller_abi, os64_js_outcome_t *outcome);

/* Extends registry settings with a separate host task queue. Configure before
 * execution. Promise checkpoints do not run cleanup; the host polls and calls
 * run_finalization_task between ordinary tasks. Each call visits one queued
 * cleanup (one cleared cell), then checkpoints its microtasks. Pending tasks
 * retain the registry and cells remain unregisterable until delivery. */
os64_js_status_t os64_js_enable_finalization_tasks(os64_js_runtime_t *runtime,
    const char *caller_abi, os64_js_outcome_t *outcome);
/* Explicit browser job policy, configured before execution: composes neutral
 * Promise settings and deferred cleanup. Abrupt neutral jobs retain outcome
 * diagnostics but do not call a realm's exception hook without a report global.
 * Other constructors/opt-ins preserve their reporting policy. */
os64_js_status_t os64_js_enable_browser_jobs(os64_js_runtime_t *runtime,
    const char *caller_abi, os64_js_outcome_t *outcome);
/* Owner-thread, allocation-free presence query; false for invalid ABI/runtime
 * or absent opt-in. Includes ineligible entries that dispatch will discard. */
bool os64_js_finalization_task_pending(os64_js_runtime_t *runtime, const char *caller_abi);
/* Requires idle execution with no open task or microtasks. Empty queue is OK.
 * Uses registry activity as task owner and callback realm as settings entry;
 * exceptions report to the registry host. Outcome jobs_pending describes only
 * microtasks; use the separate presence query for remaining cleanup tasks.
 * Ineligible entries are discarded before budget consumption. */
/* Split host delivery: visits one cleanup and leaves its task open for host
 * reporting/checkpoints under the same budget. Initialize begun to false;
 * admission writes it, pre-admission refusal leaves it unchanged. If true,
 * call task_end even after an exception/sticky failure. Empty queue opens none. */
os64_js_status_t os64_js_finalization_task_begin(os64_js_runtime_t *runtime,
    const char *caller_abi, bool *begun, os64_js_outcome_t *outcome);
os64_js_status_t os64_js_run_finalization_task(os64_js_runtime_t *runtime,
    const char *caller_abi, os64_js_outcome_t *outcome);

/* Extends finalization/Promise settings to current-realm import base fallback.
 * Configure before execution. A realm with no provider refuses null-record
 * imports; imports with active script records keep their source identity. */
os64_js_status_t os64_js_enable_import_settings(os64_js_runtime_t *runtime,
    const char *caller_abi, os64_js_outcome_t *outcome);
/* Extends import settings to empty-handler Promise reactions. Actual invoked
 * code supplies current/script incumbent; missing entry/backup remains absent.
 * Configure before execution. Generic raw jobs do not gain checked settings. */
os64_js_status_t os64_js_enable_neutral_promise_settings(os64_js_runtime_t *runtime,
    const char *caller_abi, os64_js_outcome_t *outcome);
/* Native provider: return an owned immutable URL string or JS_EXCEPTION.
 * Runs inside the existing guard after argument conversions, with checked
 * current settings; no author code. Keep opaque alive until detached or runtime
 * destruction. First installation may occur during private setup; replacements
 * and detachment require idle execution (failed-realm recovery may detach).
 * Clear with a NULL provider and opaque, including after retirement/failure. */
typedef JSValue os64_js_import_base_t(JSContext *current, void *opaque);
os64_js_status_t os64_js_realm_set_import_base(os64_js_realm_t *realm,
    const char *caller_abi, os64_js_import_base_t *base, void *opaque,
    os64_js_outcome_t *outcome);

/* Task-owned reporting call. The binding independently supplies associated
 * entry and error-reporting realm; captured incumbent is checked separately.
 * Holds value through reentry and reports exceptions as realm_call does.
 * No checkpoint, budget renewal or record ownership transfer. */
os64_js_status_t os64_js_callback_call(os64_js_realm_t *report,
    os64_js_realm_t *entry, const char *caller_abi, const os64_js_callback_t *record,
    JSValue this_value, int argc, JSValue *argv, os64_js_outcome_t *outcome);
#endif
