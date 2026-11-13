/* The host owns native/provider exclusion. The engine fence excludes executable
 * reentry while the affected set is published and native preparation runs. */
static os64_js_status_t recovery_ready(os64_js_runtime_t *runtime, const char *abi,
                                       os64_js_outcome_t *out)
{
    if (out == NULL) return OS64_JS_BAD_ARGUMENT;
    if (outcome_in_use(runtime, out)) return OS64_JS_BUSY;
    os64_memset(out, 0, sizeof(*out));
    if (runtime == NULL || abi == NULL)
        return status(out, OS64_JS_BAD_ARGUMENT, "runtime and ABI are required");
    if (os64_strcmp(abi, OS64_JS_ABI_ID) != 0)
        return status(out, OS64_JS_ABI_MISMATCH, "binding header ABI mismatch");
    if (runtime->destroying) return status(out, OS64_JS_FAILED_RUNTIME, "runtime is being destroyed");
    if (!runtime->recovery_enabled)
        return status(out, OS64_JS_ADMISSION_REFUSED, "native recovery policy is not configured");
    return OS64_JS_OK;
}

static bool recovery_wrapper_idle(os64_js_runtime_t *runtime)
{
    return !runtime->active && !runtime->task && !runtime->turn && !runtime->checkpointing &&
        !runtime->setup_calls && !runtime->setup_realm && !runtime->settings_calls;
}

static bool recovery_engine_idle(os64_js_runtime_t *runtime, bool fenced)
{
    JSExecutionState state;
    return JS_GetExecutionState(runtime->engine, &state) && state.tracking && state.blocked == fenced &&
        !state.scopes && !state.interpreter && !state.native_callbacks && !state.settings && !state.gc;
}

static bool recovery_bookkeeping_clear(os64_js_runtime_t *runtime)
{
    if (JS_IsJobPending(runtime->engine) || runtime->rejections != NULL) return false;
    for (size_t i = 0; i < runtime->realm_count; i++)
        if (runtime->realms[i].modules != NULL) return false;
    return true;
}

static os64_js_status_t recovery_global(os64_js_runtime_t *runtime, os64_js_outcome_t *out,
                                        const char *message)
{
    runtime->recovery_state = OS64_JS_RECOVERY_GLOBAL;
    out->limit = runtime->failure_record.global.limit;
    out->host_error = runtime->failure_record.global.host_error;
    out->jobs_executed = runtime->jobs;
    out->jobs_discarded = runtime->discarded_jobs;
    out->jobs_pending = JS_IsJobPending(runtime->engine);
    return status(out, runtime->failure_record.global.status == OS64_JS_OK
                  ? OS64_JS_ADMISSION_REFUSED : runtime->failure_record.global.status, message);
}

JS_PUBLIC os64_js_status_t os64_js_create_with_child_recovery(const os64_js_config_t *config,
    os64_js_teardown_policy_t policy, const char *caller_abi,
    const os64_js_recovery_host_t *host, os64_js_runtime_t **out, os64_js_outcome_t *outcome)
{
    if (out != NULL) *out = NULL;
    if (outcome == NULL) return OS64_JS_BAD_ARGUMENT;
    os64_memset(outcome, 0, sizeof(*outcome));
    if (host == NULL || host->prepare == NULL)
        return status(outcome, OS64_JS_BAD_ARGUMENT, "native recovery preparation is required");
    os64_js_status_t result = os64_js_create_with_failure_records(config, policy, caller_abi, out, outcome);
    if (result == OS64_JS_OK) {
        (*out)->recovery_host = *host;
        (*out)->recovery_enabled = true;
    }
    return result;
}

JS_PUBLIC os64_js_status_t os64_js_recovery_inspect(os64_js_runtime_t *runtime,
    const char *caller_abi, os64_js_recovery_info_t *info)
{
    if (runtime == NULL || caller_abi == NULL || info == NULL) return OS64_JS_BAD_ARGUMENT;
    if (os64_strcmp(caller_abi, OS64_JS_ABI_ID) != 0) return OS64_JS_ABI_MISMATCH;
    if (runtime->destroying) return OS64_JS_FAILED_RUNTIME;
    if (!runtime->recovery_enabled) return OS64_JS_ADMISSION_REFUSED;
    observe(runtime);
    *info = (os64_js_recovery_info_t){
        .state = runtime->recovery_state, .generation = runtime->recovery_generation,
        .failure = runtime->failure_record,
        .jobs_executed = runtime->recovery_finalized ? runtime->recovery_final_jobs :
            runtime->failure_record.turn_open ? runtime->jobs : 0,
        .jobs_discarded = runtime->recovery_finalized ? runtime->recovery_final_discarded :
            runtime->failure_record.turn_open ? runtime->discarded_jobs : 0,
        .task_open = runtime->task,
        .turn_open = runtime->turn, .jobs_pending = JS_IsJobPending(runtime->engine)
    };
    return OS64_JS_OK;
}

JS_PUBLIC os64_js_status_t os64_js_recovery_prepare(os64_js_runtime_t *runtime,
    const char *caller_abi, os64_js_recovery_token_t *token, os64_js_outcome_t *outcome)
{
    os64_js_status_t result = recovery_ready(runtime, caller_abi, outcome);
    if (result != OS64_JS_OK) return result;
    if (token == NULL) return status(outcome, OS64_JS_BAD_ARGUMENT, "token output is required");
    if (!recovery_wrapper_idle(runtime)) return status(outcome, OS64_JS_BUSY, "failed task or native guard is still open");
    observe(runtime);
    if (runtime->recovery_state != OS64_JS_RECOVERY_ABORTED && runtime->recovery_state != OS64_JS_RECOVERY_WAITING)
        return runtime->recovery_state == OS64_JS_RECOVERY_GLOBAL
            ? recovery_global(runtime, outcome, "failure requires root cleanup")
            : status(outcome, OS64_JS_ADMISSION_REFUSED, "no supported abandoned task");
    if (!runtime->recovery_finalized) {
        runtime->recovery_final_jobs = runtime->jobs;
        runtime->recovery_final_discarded = runtime->discarded_jobs;
        runtime->recovery_finalized = true;
    }
    if (!recovery_capture_eligible(runtime) || !recovery_bookkeeping_clear(runtime) ||
        JS_HasException(primary(runtime)->context))
        return recovery_global(runtime, outcome, "recovery attribution or pending bookkeeping is unsupported");
    bool waiting = runtime->recovery_state == OS64_JS_RECOVERY_WAITING;
    if (waiting) {
        if (!recovery_engine_idle(runtime, true))
            return status(outcome, OS64_JS_BUSY, "native cleanup is still active");
    } else {
        if (!recovery_engine_idle(runtime, false) ||
            !JS_BeginRecoveryBarrier(runtime->engine, &runtime->recovery_barrier))
            return status(outcome, OS64_JS_BUSY, "engine/native entry has not unwound");
        for (size_t i = 0; i < runtime->failure_record.realm_count; i++) {
            os64_js_realm_t *realm = runtime->failure_record.realms[i];
            if (!JS_MarkContextExecutableFailed(realm->context)) {
                latch(runtime, OS64_JS_HOST_FAILURE, OS64_JS_LIMIT_NONE, 0);
                return recovery_global(runtime, outcome, "engine containment refused after fence admission");
            }
            realm->failed = true;
            realm->retired = true;
        }
    }
    runtime->recovery_state = OS64_JS_RECOVERY_PREPARING;
    os64_js_failure_record_t failure = runtime->failure_record;
    os64_js_recovery_token_t prepared = {runtime, runtime->recovery_generation};
    runtime->active = true;
    runtime->active_outcome = outcome;
    int64_t error = 0;
    os64_js_status_t native = runtime->recovery_host.prepare(runtime->recovery_host.opaque, &prepared, &failure, &error);
    runtime->active_outcome = NULL;
    runtime->active = false;
    if (native != OS64_JS_OK && native != OS64_JS_BUSY)
        latch(runtime, OS64_JS_HOST_FAILURE, OS64_JS_LIMIT_NONE, error);
    observe(runtime);
    if (runtime->failure_record.global.status != OS64_JS_OK)
        return recovery_global(runtime, outcome, "native preparation observed global failure");
    if (!recovery_wrapper_idle(runtime) || !recovery_engine_idle(runtime, true) ||
        !recovery_bookkeeping_clear(runtime) || JS_HasException(primary(runtime)->context)) {
        latch(runtime, OS64_JS_HOST_FAILURE, OS64_JS_LIMIT_NONE, 0);
        return recovery_global(runtime, outcome, "native preparation left unproved work");
    }
    if (native == OS64_JS_BUSY) {
        runtime->recovery_state = OS64_JS_RECOVERY_WAITING;
        outcome->jobs_executed = runtime->recovery_final_jobs;
        outcome->jobs_discarded = runtime->recovery_final_discarded;
        return status(outcome, OS64_JS_BUSY, "native loans or cleanup have not completed");
    }
    runtime->recovery_state = OS64_JS_RECOVERY_PREPARED;
    *token = prepared;
    outcome->jobs_executed = runtime->jobs;
    outcome->jobs_discarded = runtime->discarded_jobs;
    return OS64_JS_OK;
}

JS_PUBLIC os64_js_status_t os64_js_recovery_commit(os64_js_runtime_t *runtime,
    const char *caller_abi, const os64_js_recovery_token_t *token, os64_js_outcome_t *outcome)
{
    os64_js_status_t result = recovery_ready(runtime, caller_abi, outcome);
    if (result != OS64_JS_OK) return result;
    if (token == NULL) return status(outcome, OS64_JS_BAD_ARGUMENT, "prepared token is required");
    if (token->runtime != runtime || token->generation != runtime->recovery_generation)
        return status(outcome, OS64_JS_ADMISSION_REFUSED, "stale or foreign recovery token");
    if (!recovery_wrapper_idle(runtime)) return status(outcome, OS64_JS_BUSY, "native guard is still open");
    observe(runtime);
    if (runtime->recovery_state != OS64_JS_RECOVERY_PREPARED)
        return runtime->recovery_state == OS64_JS_RECOVERY_GLOBAL
            ? recovery_global(runtime, outcome, "global failure prevents recovery commit")
            : status(outcome, OS64_JS_ADMISSION_REFUSED, "recovery was not prepared");
    if (!recovery_engine_idle(runtime, true)) return status(outcome, OS64_JS_BUSY, "engine/native entry is still active");
    if (!recovery_bookkeeping_clear(runtime) || JS_HasException(primary(runtime)->context)) {
        latch(runtime, OS64_JS_HOST_FAILURE, OS64_JS_LIMIT_NONE, 0);
        return recovery_global(runtime, outcome, "new queued or exceptional work prevents commit");
    }
    if (!JS_EndRecoveryBarrier(runtime->engine, &runtime->recovery_barrier)) {
        latch(runtime, OS64_JS_HOST_FAILURE, OS64_JS_LIMIT_NONE, 0);
        return recovery_global(runtime, outcome, "engine recovery fence release refused");
    }
    runtime->recovery_barrier = (JSRecoveryBarrier){0};
    JS_SetUncatchableException(primary(runtime)->context, false);
    runtime->failure = OS64_JS_OK;
    runtime->failed_limit = OS64_JS_LIMIT_NONE;
    runtime->host_error = 0;
    runtime->recovery_state = OS64_JS_RECOVERY_COMMITTED;
    outcome->jobs_executed = runtime->jobs;
    outcome->jobs_discarded = runtime->discarded_jobs;
    return OS64_JS_OK;
}
