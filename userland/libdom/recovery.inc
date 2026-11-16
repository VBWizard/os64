/* Native preparation runs behind libjs's execution fence. Provider callbacks
 * return normally in C; the DOM keeps their opaque storage through each loan. */
static bool recovery_borrowed(const os64_dom_cluster_t *cluster)
{
    if (cluster->provider_borrows || cluster->dispatching || cluster->recovery_running) return true;
    for (size_t i = 0; i < cluster->count; ++i)
        if (departure_busy(cluster->members[i]) || cluster->members[i]->wa_borrowed_floor != UINT64_MAX)
            return true;
    return false;
}

static os64_js_status_t recovery_prepare_native(void *opaque,
    const os64_js_recovery_token_t *token, const os64_js_failure_record_t *failure, int64_t *host_error)
{
    os64_dom_cluster_t *cluster = opaque;
    if (!cluster || !token || !failure || token->runtime != cluster->runtime || !cluster->recovery_enabled)
        return OS64_JS_HOST_FAILURE;
    if (cluster->recovering && cluster->recovery_generation != token->generation)
        return OS64_JS_HOST_FAILURE;
    cluster->recovering = true;
    cluster->recovery_generation = token->generation;
    cluster->recovery_completed = false;
    if (cluster->poisoned || cluster->drained || cluster->streams_used || !failure->realm_count)
        return OS64_JS_ADMISSION_REFUSED;
    os64_dom_t *affected[D_REALMS];
    if (failure->realm_count > D_REALMS) return OS64_JS_ADMISSION_REFUSED;
    for (size_t i = 0; i < failure->realm_count; ++i) {
        affected[i] = NULL;
        for (size_t j = 0; j < cluster->count; ++j)
            if (cluster->members[j]->realm == failure->realms[i]) affected[i] = cluster->members[j];
        if (!affected[i] || affected[i]->closed || affected[i]->installing ||
            !os64_js_realm_is_failed(affected[i]->realm))
            return OS64_JS_ADMISSION_REFUSED;
    }
    if (recovery_borrowed(cluster)) return OS64_JS_BUSY;
    cluster->recovery_running = true;
    for (size_t i = 0; i < failure->realm_count; ++i) affected[i]->departure_finishing = true;
    os64_js_status_t status = OS64_JS_OK;
    for (size_t i = 0; i < failure->realm_count; ++i) {
        os64_dom_t *dom = affected[i];
        *host_error = d_storage_drain(dom);
        if (*host_error != 0) { status = OS64_JS_HOST_FAILURE; break; }
        d_fetch_drain(dom);
        os64_js_outcome_t out;
        status = os64_js_realm_set_module_loader(dom->realm, OS64_JS_ABI_ID, NULL, &out);
        if (status == OS64_JS_OK)
            status = os64_js_realm_set_import_base(dom->realm, OS64_JS_ABI_ID, NULL, NULL, &out);
        if (status == OS64_JS_OK)
            status = os64_js_realm_set_script_host(dom->realm, OS64_JS_ABI_ID, NULL, &out);
        if (status != OS64_JS_OK) break;
        departure_activity_cancel(dom);
        d_resizes_drain(dom);
    }
    for (size_t i = 0; i < failure->realm_count; ++i) affected[i]->departure_finishing = false;
    cluster->recovery_running = false;
    if (status == OS64_JS_OK) {
        if (cluster->poisoned || cluster->drained || cluster->streams_used) status = OS64_JS_ADMISSION_REFUSED;
        else if (recovery_borrowed(cluster)) status = OS64_JS_BUSY;
    }
    if (status == OS64_JS_OK) {
        for (size_t i = 0; i < failure->realm_count; ++i) {
            departure_detach(affected[i]);
            affected[i]->departure_finished = true;
        }
        cluster->recovery_completed = true;
    }
    return status;
}

os64_js_status_t os64_dom_cluster_recover(os64_dom_cluster_t *cluster, const char *caller_abi,
    os64_js_outcome_t *outcome)
{
    os64_js_runtime_t *runtime = cluster ? cluster->runtime : NULL;
    if (cluster && !cluster->recovery_enabled) {
        /* A custom libjs policy does not certify DOM provider ownership. The
         * public query protects active outcome storage before any refusal. */
        os64_js_status_t selected = os64_js_quiescent(runtime, caller_abi, outcome);
        if (selected != OS64_JS_OK) return selected;
        return outcome->status = OS64_JS_ADMISSION_REFUSED;
    }
    os64_js_recovery_token_t token = {0};
    /* Libjs checks borrowed outcome storage and active guards before writing it. */
    os64_js_status_t status = os64_js_recovery_prepare(runtime, caller_abi, &token, outcome);
    if (status == OS64_JS_ADMISSION_REFUSED && cluster && cluster->recovery_enabled &&
        cluster->recovery_completed) {
        os64_js_recovery_info_t info;
        if (os64_js_recovery_inspect(runtime, caller_abi, &info) == OS64_JS_OK &&
            info.generation == cluster->recovery_generation) {
            if (info.state == OS64_JS_RECOVERY_COMMITTED) {
                if (recovery_borrowed(cluster)) return outcome->status = OS64_JS_BUSY;
                cluster->recovering = false;
                os64_memset(outcome, 0, sizeof(*outcome));
                outcome->jobs_executed = info.jobs_executed;
                outcome->jobs_discarded = info.jobs_discarded;
                return OS64_JS_OK;
            }
            if (info.state == OS64_JS_RECOVERY_PREPARED) {
                token = (os64_js_recovery_token_t){runtime, info.generation};
                status = OS64_JS_OK;
            }
        }
    }
    if (status != OS64_JS_OK) return status;
    /* A native caller can prepare separately and release its loans later. */
    if (recovery_borrowed(cluster)) return outcome->status = OS64_JS_BUSY;
    status = os64_js_recovery_commit(runtime, caller_abi, &token, outcome);
    if (status == OS64_JS_OK && cluster->recovery_completed &&
        cluster->recovery_generation == token.generation)
        cluster->recovering = false;
    return status;
}

os64_dom_recovery_result_t os64_dom_cluster_resolve_failure(os64_dom_cluster_t *cluster,
    const char *caller_abi, uint64_t generation, const os64_js_outcome_t *failed_task)
{
    os64_dom_recovery_result_t result = {.action=OS64_DOM_RECOVERY_ROOT_REQUIRED,
        .status=OS64_JS_BAD_ARGUMENT, .generation=generation};
    if (!cluster || !failed_task || !caller_abi) return result;
    if (os64_strcmp(caller_abi, OS64_JS_ABI_ID) != 0) {
        result.status = OS64_JS_ABI_MISMATCH;
        return result;
    }
    if (failed_task->status != OS64_JS_LIMIT && failed_task->status != OS64_JS_CANCELLED &&
        failed_task->status != OS64_JS_HOST_FAILURE && failed_task->status != OS64_JS_FAILED_RUNTIME)
        return result;
    result.status = failed_task->status;
    result.limit = failed_task->limit;
    result.host_error = failed_task->host_error;
    if (failed_task->status != OS64_JS_LIMIT || failed_task->limit != OS64_JS_LIMIT_EXECUTION)
        return result;
    if (!cluster->recovery_enabled) {
        result.status = OS64_JS_ADMISSION_REFUSED;
        return result;
    }
    result.limit = OS64_JS_LIMIT_NONE;
    result.host_error = 0;
    os64_js_recovery_info_t info;
    result.status = os64_js_recovery_inspect(cluster->runtime, caller_abi, &info);
    if (result.status != OS64_JS_OK) return result;
    /* A new global episode can supersede a completed local generation. */
    if (info.failure.global.status != OS64_JS_OK) {
        result.status = info.failure.global.status;
        result.limit = info.failure.global.limit;
        result.host_error = info.failure.global.host_error;
        return result;
    }
    if (!generation || info.generation != generation ||
        info.failure.original.status != failed_task->status ||
        info.failure.original.limit != failed_task->limit ||
        info.failure.original.host_error != failed_task->host_error) {
        result.status = OS64_JS_ADMISSION_REFUSED;
        return result;
    }
    os64_js_outcome_t admission;
    result.status = os64_dom_cluster_recover(cluster, caller_abi, &admission);
    result.limit = admission.limit;
    result.host_error = admission.host_error;
    if (result.status == OS64_JS_BUSY) result.action = OS64_DOM_RECOVERY_WAIT;
    else if (result.status == OS64_JS_OK) {
        /* A late global signal still wins after native publication. Normal
         * task admission repeats the health check for a later cancellation. */
        result.status = os64_js_recovery_inspect(cluster->runtime, caller_abi, &info);
        if (result.status == OS64_JS_OK && info.generation == generation &&
            info.state == OS64_JS_RECOVERY_COMMITTED && info.failure.global.status == OS64_JS_OK)
            result.action = OS64_DOM_RECOVERY_READY;
        else if (result.status == OS64_JS_OK) {
            result.status = info.failure.global.status == OS64_JS_OK
                ? OS64_JS_ADMISSION_REFUSED : info.failure.global.status;
            result.limit = info.failure.global.limit;
            result.host_error = info.failure.global.host_error;
        }
    }
    return result;
}
