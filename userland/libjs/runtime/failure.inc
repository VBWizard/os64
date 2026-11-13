/* Visitors copy into native scratch; engine/runtime state is unchanged until
 * the visit returns. Context membership never trusts a context's opaque slot. */
typedef struct {
    os64_js_runtime_t *runtime;
    os64_js_failure_record_t record;
    size_t scopes;
} FailureCapture;

static os64_js_realm_t *failure_context(FailureCapture *capture, JSContext *context)
{
    if (context == NULL) return NULL;
    os64_js_realm_t *realm = realm_for_context(capture->runtime, context);
    if (realm == NULL) capture->record.scope_issues |= OS64_JS_FAILURE_FOREIGN_CONTEXT;
    return realm;
}

static void failure_scope(void *opaque, const JSExecutionScopeInfo *info)
{
    FailureCapture *capture = opaque;
    capture->scopes++;
    failure_context(capture, info->caller);
    os64_js_realm_t *realm = failure_context(capture, info->code);
    if (info->kind == JS_EXECUTION_SCOPE_JOB) {
        capture->record.scope_issues |= OS64_JS_FAILURE_JOB_SCOPE | OS64_JS_FAILURE_UNKNOWN_CODE;
        return;
    }
    if (info->code_kind == JS_EXECUTION_CODE_NATIVE) {
        capture->record.scope_issues |= OS64_JS_FAILURE_NATIVE_SCOPE;
        return;
    }
    if (info->code_kind == JS_EXECUTION_CODE_UNKNOWN || info->code == NULL)
        capture->record.scope_issues |= OS64_JS_FAILURE_UNKNOWN_CODE;
    if (realm == NULL) return;
    for (size_t i = 0; i < capture->record.realm_count; i++)
        if (capture->record.realms[i] == realm) return;
    if (capture->record.realm_count == OS64_JS_MAX_REALMS)
        capture->record.scope_issues |= OS64_JS_FAILURE_SCOPE_OVERFLOW;
    else
        capture->record.realms[capture->record.realm_count++] = realm;
}

static void failure_capture(os64_js_runtime_t *runtime, os64_js_status_t value,
                             os64_js_limit_t limit, int64_t error)
{
    FailureCapture capture = {.runtime = runtime};
    capture.record.original = (os64_js_failure_cause_t){value, limit, error};
    capture.record.turn_open = runtime->turn;
    capture.record.explicit_task = runtime->task;
    capture.record.deadline = runtime->turn ? runtime->deadline : 0;
    capture.record.task_owner = runtime->turn ? runtime->turn_owner : NULL;
    capture.record.jobs_executed = runtime->turn ? runtime->jobs : 0;
    capture.record.jobs_discarded = runtime->turn ? runtime->discarded_jobs : 0;
    if (runtime->setup_calls != NULL || runtime->setup_realm != NULL)
        capture.record.scope_issues |= OS64_JS_FAILURE_SETUP_SCOPE;
    if (runtime->settings_calls != NULL)
        capture.record.scope_issues |= OS64_JS_FAILURE_SETTINGS_SCOPE;
    if (runtime->failure_job != NULL) {
        capture.record.job_execution = failure_context(&capture, runtime->failure_job->context);
        capture.record.job_callback = failure_context(&capture, runtime->failure_job->owner);
        capture.record.job_activity = failure_context(&capture, runtime->failure_job->activity);
        capture.record.scope_issues |= OS64_JS_FAILURE_JOB_SCOPE;
    }
    if (!JS_VisitExecutionScopes(runtime->engine, failure_scope, &capture))
        capture.record.scope_issues |= OS64_JS_FAILURE_UNKNOWN_CODE;
    if (capture.scopes == 0) capture.record.scope_issues |= OS64_JS_FAILURE_NO_SCOPE;
    capture.record.scopes_classified = capture.record.scope_issues == 0;
    runtime->failure_record = capture.record;
}
