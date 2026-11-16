os64_page_retention_t os64_page_retention_snapshot(const os64_page_state_t *state,
    size_t scan_limit)
{
    os64_page_retention_t result = {.unknown=OS64_PAGE_RETENTION_UNKNOWN_INCOMING |
        OS64_PAGE_RETENTION_UNKNOWN_EXPORTS | OS64_PAGE_RETENTION_UNKNOWN_GROUP_BORROWS |
        OS64_PAGE_RETENTION_UNKNOWN_RETIRED_BYTES | OS64_PAGE_RETENTION_UNKNOWN_MODEL_REFERENCES |
        OS64_PAGE_RETENTION_UNKNOWN_MODELS};
    if (!state) return result;
    size_t limit = scan_limit < OS64_PAGE_RETENTION_SCAN_MAX ? scan_limit : OS64_PAGE_RETENTION_SCAN_MAX;
    const os64_page_t *model = state->models;
    while (model && result.models_seen < limit) {
        result.models_seen++;
        model = model->state_next;
    }
    result.models_complete = model == NULL;
    if (result.models_complete) result.unknown &= ~OS64_PAGE_RETENTION_UNKNOWN_MODELS;
    result.valid = true;
    result.physical_bytes = state->bytes;
    result.grouped = state->group != NULL;
    result.private_owner = state->private_owner;
    return result;
}
