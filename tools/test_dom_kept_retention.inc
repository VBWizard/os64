#include <dom/kept_retention.h>

static void dom_kept_retention_native_cases(DomRecoveryFixture *f)
{
    os64_dom_t *dom = f->binding[1];
    os64_dom_kept_retention_t empty = os64_dom_kept_retention_snapshot(dom, OS64_JS_ABI_ID, 0);
    check(empty.status == OS64_JS_OK && empty.records_complete && !empty.records_seen &&
        !empty.quota_bytes && empty.unknown == (OS64_DOM_KEPT_UNKNOWN_ALL & ~OS64_DOM_KEPT_UNKNOWN_RECORDS),
        "kept retention: empty local list keeps incoming and other payload coverage unknown");
    check(os64_dom_kept_retention_snapshot(NULL, OS64_JS_ABI_ID, 0).status == OS64_JS_BAD_ARGUMENT &&
        os64_dom_kept_retention_snapshot(dom, NULL, 0).status == OS64_JS_BAD_ARGUMENT &&
        os64_dom_kept_retention_snapshot(dom, "wrong ABI", SIZE_MAX).status == OS64_JS_ABI_MISMATCH,
        "kept retention: invalid arguments and ABI refuse data");
    DKept *records[65];
    for (size_t i = 0; i < 65; i++) {
        records[i] = d_kept_new(dom, 0);
        check(records[i] != NULL, "kept retention: allocate uncharged empty response record");
    }
    os64_dom_kept_retention_t probe = os64_dom_kept_retention_snapshot(dom, OS64_JS_ABI_ID, 0);
    check(!probe.quota_bytes && !probe.records_seen && !probe.records_complete &&
        probe.unknown == OS64_DOM_KEPT_UNKNOWN_ALL,
        "kept retention: zero quota and zero prefix do not mean no native records");
    probe = os64_dom_kept_retention_snapshot(dom, OS64_JS_ABI_ID, 1);
    check(probe.records_seen == 1 && probe.empty_records == 1 && !probe.records_complete,
        "kept retention: caller budget limits the prefix");
    probe = os64_dom_kept_retention_snapshot(dom, OS64_JS_ABI_ID, SIZE_MAX);
    check(probe.records_seen == 64 && probe.empty_records == 64 && !probe.records_complete,
        "kept retention: oversized budget clamps at 64 records");
    d_kept_unref(records[64]);
    probe = os64_dom_kept_retention_snapshot(dom, OS64_JS_ABI_ID, SIZE_MAX);
    check(probe.records_seen == 64 && probe.records_complete && probe.max_record_references == 1 &&
        !probe.shared_records && !probe.quota_bytes_seen,
        "kept retention: exact boundary completes without inferring released ownership");
    for (size_t i = 0; i < 64; i++) d_kept_unref(records[i]);

    DKept *blob = d_kept_new_whole(dom, 0), *body = d_kept_new(dom, 3);
    check(blob && body && d_kept_append(body, (const uint8_t *)"abc", 3) &&
        d_kept_append(body, (const uint8_t *)"de", 2), "kept retention: block and segmented body fixture");
    d_kept_ref(body); d_kept_ref(body);
    probe = os64_dom_kept_retention_snapshot(dom, OS64_JS_ABI_ID, SIZE_MAX);
    check(probe.records_seen == 2 && probe.records_complete && probe.empty_records == 1 &&
        probe.shared_records == 1 && probe.max_record_references == 3 &&
        probe.segmented_records == 1 && probe.block_records == 1 &&
        probe.quota_bytes == probe.quota_bytes_seen && probe.quota_bytes > 5,
        "kept retention: native references and charged block/piece/header storage are descriptive");
    os64_dom_kept_retention_t zero = os64_dom_kept_retention_snapshot(dom, OS64_JS_ABI_ID, 0);
    check(zero.quota_bytes == probe.quota_bytes && !zero.quota_bytes_seen && !zero.records_complete,
        "kept retention: full quota stays visible when the charged prefix is empty");
    size_t allocated = attempts, native_allocated = dom_attempts, old_live = dom_live;
    int64_t now = dom_clock;
    fail_at = allocated + 1; dom_fail_at = native_allocated + 1; dom_clock_step = 5000000;
    for (size_t i = 0; i < 100; i++) {
        os64_dom_kept_retention_t again = os64_dom_kept_retention_snapshot(dom, OS64_JS_ABI_ID, SIZE_MAX);
        check(again.quota_bytes == probe.quota_bytes && again.records_seen == 2 &&
            again.shared_records == 1 && again.max_record_references == 3 && again.segmented_records == 1,
            "kept retention: inspection neither joins bytes nor consumes references");
    }
    fail_at = dom_fail_at = 0; dom_clock_step = 0;
    check(allocated == attempts && native_allocated == dom_attempts && old_live == dom_live && now == dom_clock,
        "kept retention: queries allocate/free nothing and do not poll a clock");
    check(d_kept_join(body) && !memcmp(d_kept_bytes(body), "abcde", 5),
        "kept retention: explicit join still produces the original bytes");
    os64_dom_kept_retention_t joined = os64_dom_kept_retention_snapshot(dom, OS64_JS_ABI_ID, SIZE_MAX);
    check(!joined.segmented_records && joined.block_records == 1 && joined.quota_bytes < probe.quota_bytes &&
        joined.max_record_references == 3, "kept retention: join changes storage without dropping references");
    d_kept_unref(body); d_kept_unref(body);
    check(!os64_dom_kept_retention_snapshot(dom, OS64_JS_ABI_ID, SIZE_MAX).shared_records,
        "kept retention: explicit nonfinal releases preserve the body");
    d_kept_unref(body); d_kept_unref(blob);
    probe = os64_dom_kept_retention_snapshot(dom, OS64_JS_ABI_ID, SIZE_MAX);
    check(probe.records_complete && !probe.records_seen && !probe.quota_bytes && probe.unknown,
        "kept retention: explicit final releases restore ledger without certifying other owners");
}
