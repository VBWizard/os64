#ifndef OS64_JS_FAILURE_H
#define OS64_JS_FAILURE_H
#include "os64/js.h"

typedef struct {
    os64_js_status_t status;
    os64_js_limit_t limit;
    int64_t host_error;
} os64_js_failure_cause_t;

typedef enum {
    OS64_JS_FAILURE_NO_SCOPE = 1u << 0,
    OS64_JS_FAILURE_UNKNOWN_CODE = 1u << 1,
    OS64_JS_FAILURE_FOREIGN_CONTEXT = 1u << 2,
    OS64_JS_FAILURE_NATIVE_SCOPE = 1u << 3,
    OS64_JS_FAILURE_JOB_SCOPE = 1u << 4,
    OS64_JS_FAILURE_SETUP_SCOPE = 1u << 5,
    OS64_JS_FAILURE_SETTINGS_SCOPE = 1u << 6,
    OS64_JS_FAILURE_SCOPE_OVERFLOW = 1u << 7
} os64_js_failure_scope_issue_t;

/* First-failure facts are frozen before unwind. Realm handles are borrowed
 * until runtime destruction, including retired realms. Code candidates are
 * deduplicated; an incomplete set must not be used to authorize containment.
 * Native defining contexts and job ownership are not author-code identity.
 * scopes_classified describes the observed engine scopes, not native safety,
 * task closure, queue consistency or permission to recover. */
typedef struct {
    os64_js_failure_cause_t original;
    /* First non-EXECUTION terminal cause observed, including after the original
     * timeout. OK means no such signal observed; the runtime remains terminal. */
    os64_js_failure_cause_t global;
    int64_t deadline; /* original armed deadline; zero without an open turn */
    uint64_t jobs_executed, jobs_discarded; /* counts at first failure */
    os64_js_realm_t *task_owner; /* can name an unpublished setup slot; diagnostic only */
    os64_js_realm_t *realms[OS64_JS_MAX_REALMS];
    size_t realm_count;
    unsigned scope_issues;
    bool turn_open, explicit_task, scopes_classified;
    /* Wrapper-dispatched job provenance, separate from the code candidate set.
     * NULL can mean neutral ownership or no wrapper job; JOB_SCOPE identifies
     * an observed job scope even when raw dispatch bypasses the wrapper.
     * Nested raw jobs retain the enclosing wrapper dispatch's provenance. */
    os64_js_realm_t *job_execution, *job_callback, *job_activity;
} os64_js_failure_record_t;

/* Opt-in diagnostics, enabled during construction before engine exposure.
 * Uses the existing teardown policies and reserves inline record storage
 * against the memory ceiling. No failure reset or recovery is enabled. */
os64_js_status_t os64_js_create_with_failure_records(const os64_js_config_t *config,
    os64_js_teardown_policy_t policy, const char *caller_abi,
    os64_js_runtime_t **out, os64_js_outcome_t *outcome);

/* Owner thread, including native callbacks and after failure/task closure.
 * Polls cancellation/allocator state and, while no global cause is recorded,
 * the clock after EXECUTION too. Copies the record without allocation, script
 * execution, exception consumption, job draining or budget renewal. Returns OK
 * for a successful observation; original/global hold the failure verdicts.
 * Disabled runtimes return ADMISSION_REFUSED. Refusal leaves record unchanged.
 * Separate record storage is required; do not alias active outcome storage. */
os64_js_status_t os64_js_observe_failure(os64_js_runtime_t *runtime,
    const char *caller_abi, os64_js_failure_record_t *record);
#endif
