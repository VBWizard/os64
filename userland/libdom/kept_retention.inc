os64_dom_kept_retention_t os64_dom_kept_retention_snapshot(const os64_dom_t *dom,
    const char *caller_abi, size_t scan_limit)
{
    os64_dom_kept_retention_t result = {.status=OS64_JS_BAD_ARGUMENT,
        .unknown=OS64_DOM_KEPT_UNKNOWN_ALL};
    if (!dom || !caller_abi) return result;
    if (os64_strcmp(caller_abi, OS64_JS_ABI_ID) != 0) {
        result.status = OS64_JS_ABI_MISMATCH;
        return result;
    }
    size_t limit = scan_limit < OS64_DOM_KEPT_RETENTION_SCAN_MAX ?
        scan_limit : OS64_DOM_KEPT_RETENTION_SCAN_MAX;
    const DKept *body = dom->kept;
    while (body && result.records_seen < limit) {
        result.records_seen++;
        /* Disjoint record charges are a subset of the quota ledger. */
        result.quota_bytes_seen += body->charged;
        result.shared_records += body->refs > 1;
        result.empty_records += body->length == 0;
        result.segmented_records += body->pieces != NULL;
        result.block_records += body->block != NULL;
        if (body->refs > result.max_record_references)
            result.max_record_references = body->refs;
        body = body->next;
    }
    result.records_complete = body == NULL;
    if (result.records_complete) result.unknown &= ~OS64_DOM_KEPT_UNKNOWN_RECORDS;
    result.quota_bytes = dom->body_bytes;
    result.closed = dom->closed;
    result.status = OS64_JS_OK;
    return result;
}
