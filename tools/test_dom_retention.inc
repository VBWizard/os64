#include <dom/retention.h>
#include <os64/js_retention.h>

static void dom_retention_cases(void)
{
    for (unsigned policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; ++policy) {
        DomRecoveryFixture f;
        if (!dom_recovery_open(&f, policy, true)) continue;
        size_t allocated = attempts, native_allocated = dom_attempts;
        int64_t now = dom_clock;
        fail_at = allocated+1; dom_fail_at = native_allocated+1; dom_clock_step = 5000000;
        os64_dom_retention_t native = os64_dom_retention_snapshot(f.cluster, OS64_JS_ABI_ID);
        os64_js_retention_t wrapper = os64_js_retention_snapshot(f.runtime, OS64_JS_ABI_ID);
        fail_at = dom_fail_at = 0; dom_clock_step = 0;
        check(attempts == allocated && dom_attempts == native_allocated && dom_clock == now &&
              native.status == OS64_JS_OK && wrapper.status == OS64_JS_OK,
              "retention: wrapper/native queries allocate nothing and never poll a stepping clock");
        dom_clock = now;
        os64_dom_cluster_limits_t bytes = os64_dom_cluster_bytes(f.cluster);
        check(native.html_bytes == bytes.html_bytes && native.page_bytes == bytes.page_bytes &&
              native.dom_bytes == bytes.dom_bytes && native.kept_bytes == bytes.kept_bytes &&
              native.canvas_bytes == bytes.canvas_bytes && native.canvas_bytes &&
              native.occupied_members == 3 && native.unknown == OS64_DOM_RETENTION_UNKNOWN_ALL,
              "retention: physical native ledgers remain charged despite incomplete incoming-edge coverage");
        check(d_provider_enter(f.binding[1]), "retention: actual sibling provider loan begins");
        os64_dom_effects_mark(f.binding[2]);
        native = os64_dom_retention_snapshot(f.cluster, OS64_JS_ABI_ID);
        check(native.provider_borrows == 1 && native.layout_borrowers == 1 &&
              native.oldest_layout_generation != UINT64_MAX && d_provider_busy(f.binding[0]),
              "retention: provider and layout borrowers remain visible across sibling members");
        os64_dom_retention_t again = os64_dom_retention_snapshot(f.cluster, OS64_JS_ABI_ID);
        check(again.provider_borrows == 1 && again.layout_borrowers == 1 &&
              again.oldest_layout_generation == native.oldest_layout_generation,
              "retention: repeated inspection does not release a loan or acknowledge an effect generation");
        d_provider_leave(f.binding[1]);
        os64_dom_effects_release(f.binding[2], UINT64_MAX);
        native = os64_dom_retention_snapshot(f.cluster, OS64_JS_ABI_ID);
        check(!native.provider_borrows && !native.layout_borrowers &&
              native.oldest_layout_generation == UINT64_MAX && native.unknown == OS64_DOM_RETENTION_UNKNOWN_ALL,
              "retention: released visible loans do not erase unknown borrower families");
        os64_js_outcome_t out;
        check(os64_dom_depart(f.binding[1], &out) == OS64_JS_OK &&
              os64_dom_depart_finish(f.binding[1], &out) == OS64_JS_OK,
              "retention: ordinary document departure completes without reclaiming its owner");
        wrapper = os64_js_retention_snapshot(f.runtime, OS64_JS_ABI_ID);
        native = os64_dom_retention_snapshot(f.cluster, OS64_JS_ABI_ID);
        check(wrapper.retired_slots == 1 && !wrapper.failed_slots && wrapper.context_roots == 3 &&
              native.departure_finished_members == 1 && !native.closed_members && native.retained_value_records,
              "retention: ordinary departure retains backing separately from executable containment");
        realm_script(f.realm[0], "if(B.bad(true)!==7)throw Error('retained ordinary alias')",
                     "retention: diagnostic reads preserve callable aliases after ordinary departure");
        dom_recovery_close(&f);
        check(!live && !dom_live && !dom_allocations, "retention: both root policies release native and engine allocations");
    }
}
