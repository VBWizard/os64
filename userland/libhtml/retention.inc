os64_html_retention_t os64_html_retention_snapshot(const os64_html_document_t *doc,
    size_t scan_limit)
{
    os64_html_retention_t result = {.status=OS64_HTML_BAD_ARGUMENT,
        .unknown=OS64_HTML_RETENTION_UNKNOWN_INCOMING | OS64_HTML_RETENTION_UNKNOWN_EXPORTS |
            OS64_HTML_RETENTION_UNKNOWN_GROUP_BORROWS | OS64_HTML_RETENTION_UNKNOWN_PENDING_NODES |
            OS64_HTML_RETENTION_UNKNOWN_READERS | OS64_HTML_RETENTION_UNKNOWN_RANGES};
    if (!doc) return result;
    const HDoc *d = (const HDoc *)doc;
    size_t limit = scan_limit < OS64_HTML_RETENTION_SCAN_MAX ? scan_limit : OS64_HTML_RETENTION_SCAN_MAX;
    result.status = OS64_HTML_OK;
    result.arena_id = d->id;
    result.creation_realm = d->creation_realm;
    result.arena_bytes = doc->arena_bytes;
    result.peak_arena_bytes = doc->peak_arena_bytes;
    result.retired_block_bytes = d->retired_bytes;
    result.holds = d->held;
    result.snapshot_pins = d->pinned;
    result.readers.seen = d_retention_readers(d, limit, &result.readers.complete);
    result.associated_ranges.seen = d_retention_ranges(d, limit, &result.associated_ranges.complete);
    if (result.readers.complete) result.unknown &= ~OS64_HTML_RETENTION_UNKNOWN_READERS;
    if (result.associated_ranges.complete) result.unknown &= ~OS64_HTML_RETENTION_UNKNOWN_RANGES;
    result.grouped = d->group != NULL;
    result.charged_to_group = d->charge_group != NULL;
    result.parser_active = d->parser != NULL;
    return result;
}

os64_html_node_retention_t os64_html_node_retention_snapshot(const HNode *node,
    size_t scan_limit)
{
    os64_html_node_retention_t result = {.owner=os64_html_retention_snapshot(NULL, scan_limit)};
    if (!node) return result;
    const HMeta *meta = h_meta(node);
    result.owner = os64_html_retention_snapshot(&meta->allocation_owner->pub, scan_limit);
    result.logical_document_arena_id = d_node_document_owner(node)->id;
    result.node_nonparser_refs = meta->holds;
    result.parser_refs = meta->parser_refs;
    result.creation_realm = meta->creation_realm;
    return result;
}
