JS_PUBLIC os64_js_retention_t os64_js_retention_snapshot(const os64_js_runtime_t *runtime,
    const char *caller_abi)
{
    os64_js_retention_t result = {.status=OS64_JS_BAD_ARGUMENT,
        .unknown=OS64_JS_RETENTION_UNKNOWN_ALL};
    if (!runtime || !caller_abi) return result;
    if (os64_strcmp(caller_abi, OS64_JS_ABI_ID) != 0) {
        result.status = OS64_JS_ABI_MISMATCH;
        return result;
    }
    result.destroying = runtime->destroying;
    if (result.destroying) {
        result.status = OS64_JS_FAILED_RUNTIME;
        return result;
    }
    result.status = OS64_JS_OK;
    result.occupied_slots = runtime->realm_count;
    for (size_t i = 0; i < runtime->realm_count; ++i) {
        const os64_js_realm_t *realm = &runtime->realms[i];
        result.context_roots += realm->context != NULL;
        result.promise_then_roots += !JS_IsUndefined(realm->promise_then);
        result.retired_slots += realm->retired;
        result.failed_slots += realm->failed;
        result.staged_slots += realm->staged;
        result.module_owner_slots += realm->modules != NULL;
    }
    result.rejection_records = runtime->rejection_count;
    result.heap_bytes = runtime->allocator.charged_bytes;
    result.heap_peak = runtime->allocator.peak_bytes;
    result.latched_status = runtime->failure;
    result.latched_limit = runtime->failed_limit;
    result.allocator_failures = runtime->allocator.failures;
    result.cancelled = __atomic_load_n(&runtime->cancelled, __ATOMIC_ACQUIRE) != 0;
    result.jobs_pending = JS_IsJobPending(runtime->engine);
    result.active = runtime->active;
    result.task_open = runtime->task;
    result.turn_open = runtime->turn;
    result.setup_active = runtime->setup_calls || runtime->setup_realm;
    result.settings_active = runtime->settings_calls != NULL;
    return result;
}
