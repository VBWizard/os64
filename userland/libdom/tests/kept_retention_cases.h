#ifndef OS64_DOM_KEPT_RETENTION_CASES_H
#define OS64_DOM_KEPT_RETENTION_CASES_H
#include <dom/kept_retention.h>

static void kept_retention_script(os64_js_realm_t *realm, const char *source,
    void (*require)(bool, const char *))
{
    os64_js_outcome_t out;
    require(os64_js_realm_run(realm, source, os64_strlen(source), "kept-retention", &out) == OS64_JS_OK,
        "kept retention: script fixture runs under its original realm budget");
}
/* Public queries and script aliases shared by the host and target consumer. */
static void kept_retention_alias_case(os64_js_realm_t **realms, os64_dom_t **bindings,
    void (*require)(bool, const char *))
{
    kept_retention_script(realms[1], "globalThis.payload=new Blob(['kept'])", require);
    os64_dom_kept_retention_t source = os64_dom_kept_retention_snapshot(bindings[1], OS64_JS_ABI_ID, SIZE_MAX);
    require(source.status == OS64_JS_OK && source.records_complete && source.records_seen == 1 &&
        source.max_record_references == 1 && !source.shared_records && source.quota_bytes >= 4 &&
        source.quota_bytes_seen == source.quota_bytes,
        "kept retention: one source Blob owns physical body quota");
    kept_retention_script(realms[2], "globalThis.payloadAlias=B.payload;globalThis.payloadAliasAgain=payloadAlias", require);
    os64_dom_kept_retention_t aliases = os64_dom_kept_retention_snapshot(bindings[1], OS64_JS_ABI_ID, SIZE_MAX);
    os64_dom_kept_retention_t foreign = os64_dom_kept_retention_snapshot(bindings[2], OS64_JS_ABI_ID, SIZE_MAX);
    require(aliases.records_seen == 1 && aliases.max_record_references == 1 && !aliases.shared_records &&
        aliases.quota_bytes == source.quota_bytes && !foreign.records_seen && !foreign.quota_bytes &&
        (aliases.unknown & OS64_DOM_KEPT_UNKNOWN_INCOMING) &&
        (aliases.unknown & OS64_DOM_KEPT_UNKNOWN_REFERENCE_ROLES),
        "kept retention: foreign script aliases are not native reference counts or destination charges");
    kept_retention_script(realms[2], "globalThis.payloadSlice=payloadAlias.slice(1,3);"
        "if(payloadSlice.size!==2||payloadAlias.size!==4)throw Error('payload aliases')", require);
    aliases = os64_dom_kept_retention_snapshot(bindings[1], OS64_JS_ABI_ID, SIZE_MAX);
    require(aliases.records_seen == 1 && aliases.shared_records == 1 && aliases.max_record_references == 2 &&
        aliases.quota_bytes == source.quota_bytes,
        "kept retention: foreign Blob slice retains its original physical ledger");
}
/* Response construction enters the Streams brand-check door. Keep this
 * outside the plain child-local recovery fixture. */
static void kept_retention_response_case(os64_js_realm_t **realms, os64_dom_t **bindings,
    void (*require)(bool, const char *))
{
    os64_dom_kept_retention_t source = os64_dom_kept_retention_snapshot(bindings[1], OS64_JS_ABI_ID, SIZE_MAX);
    kept_retention_script(realms[2], "globalThis.payloadResponse=new Response(payloadAlias);"
        "if(payloadResponse.bodyUsed)throw Error('unused response')", require);
    os64_dom_kept_retention_t aliases = os64_dom_kept_retention_snapshot(bindings[1], OS64_JS_ABI_ID, SIZE_MAX);
    os64_dom_kept_retention_t foreign = os64_dom_kept_retention_snapshot(bindings[2], OS64_JS_ABI_ID, SIZE_MAX);
    require(aliases.records_seen == 1 && aliases.shared_records == 1 && aliases.max_record_references == 2 &&
        aliases.quota_bytes == source.quota_bytes && !foreign.quota_bytes && foreign.records_complete &&
        (foreign.unknown & OS64_DOM_KEPT_UNKNOWN_OTHER_PAYLOADS),
        "kept retention: constructed Response copies into engine storage outside this native census");
}
#endif
