os64_dom_retention_t os64_dom_retention_snapshot(const os64_dom_cluster_t *cluster,
    const char *caller_abi)
{
    os64_dom_retention_t result = {.status=OS64_JS_BAD_ARGUMENT,
        .unknown=OS64_DOM_RETENTION_UNKNOWN_ALL, .oldest_layout_generation=UINT64_MAX};
    if (!cluster || !caller_abi) return result;
    if (os64_strcmp(caller_abi, OS64_JS_ABI_ID) != 0) {
        result.status = OS64_JS_ABI_MISMATCH;
        return result;
    }
    result.status = OS64_JS_OK;
    result.occupied_members = cluster->count;
    for (size_t i = 0; i < cluster->count; ++i) {
        const os64_dom_t *dom = cluster->members[i];
        result.closed_members += dom->closed;
        result.installing_members += dom->installing;
        result.departure_finished_members += dom->departure_finished;
        result.retained_value_records += dom->retained;
        result.reported_registry_values += os64_dom_registry_count(dom);
        result.activity_busy_members += departure_busy(dom);
        if (dom->wa_borrowed_floor != UINT64_MAX) {
            result.layout_borrowers++;
            if (dom->wa_borrowed_floor < result.oldest_layout_generation)
                result.oldest_layout_generation = dom->wa_borrowed_floor;
        }
    }
    os64_dom_cluster_limits_t bytes = os64_dom_cluster_bytes(cluster);
    result.html_bytes = bytes.html_bytes;
    result.page_bytes = bytes.page_bytes;
    result.dom_bytes = bytes.dom_bytes;
    result.kept_bytes = bytes.kept_bytes;
    result.canvas_bytes = bytes.canvas_bytes;
    result.provider_borrows = cluster->provider_borrows;
    result.dispatch_depth = cluster->dispatching;
    result.recovery_generation = cluster->recovery_generation;
    result.drained = cluster->drained;
    result.poisoned = cluster->poisoned;
    result.recovering = cluster->recovering;
    result.recovery_running = cluster->recovery_running;
    result.recovery_completed = cluster->recovery_completed;
    result.streams_used = cluster->streams_used;
    return result;
}
