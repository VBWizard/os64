#include "../userland/libdom/tests/recovery_cases.h"
static void recovery_host_require(bool pass, const char *what)
{
    check(pass, what);
    if (!pass) abort();
}
static void recovery_host_clock(bool stepping)
{ dom_clock_step = stepping ? 10000 : 0; }
static void recovery_host_edge_cases(void)
{
    for (unsigned policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; ++policy) {
        DomRecoveryFixture f;
        if (!dom_recovery_open(&f, policy, true)) continue;
        os64_js_outcome_t original = dom_recovery_overrun(&f), saved = original;
        os64_js_recovery_info_t first;
        check(os64_js_recovery_inspect(f.runtime, OS64_JS_ABI_ID, &first) == OS64_JS_OK,
              "recovery host: explicit generation snapshot");
        os64_dom_recovery_result_t result = os64_dom_cluster_resolve_failure(f.cluster,
            "wrong ABI", first.generation, &original);
        check(result.action == OS64_DOM_RECOVERY_ROOT_REQUIRED && result.status == OS64_JS_ABI_MISMATCH,
              "recovery host: ABI mismatch never grants dispatch");
        result = os64_dom_cluster_resolve_failure(NULL, OS64_JS_ABI_ID, first.generation, &original);
        check(result.action == OS64_DOM_RECOVERY_ROOT_REQUIRED && result.status == OS64_JS_BAD_ARGUMENT,
              "recovery host: missing cluster fails closed");
        result = os64_dom_cluster_resolve_failure(f.cluster, OS64_JS_ABI_ID, first.generation, NULL);
        check(result.action == OS64_DOM_RECOVERY_ROOT_REQUIRED && result.status == OS64_JS_BAD_ARGUMENT,
              "recovery host: missing task diagnostics fail closed");
        os64_js_outcome_t other = {.status=OS64_JS_OK};
        result = os64_dom_cluster_resolve_failure(f.cluster, OS64_JS_ABI_ID, first.generation, &other);
        check(result.action == OS64_DOM_RECOVERY_ROOT_REQUIRED && result.status == OS64_JS_BAD_ARGUMENT,
              "recovery host: a nonterminal verdict cannot hide an abandoned task");
        other = (os64_js_outcome_t){.status=OS64_JS_LIMIT,.limit=OS64_JS_LIMIT_MEMORY};
        result = os64_dom_cluster_resolve_failure(f.cluster, OS64_JS_ABI_ID, first.generation, &other);
        check(result.action == OS64_DOM_RECOVERY_ROOT_REQUIRED && result.status == OS64_JS_LIMIT &&
              result.limit == OS64_JS_LIMIT_MEMORY && !os64_js_realm_is_failed(f.realm[1]),
              "recovery host: memory verdict never prepares or clears a local latch");
        result = os64_dom_cluster_resolve_failure(f.cluster, OS64_JS_ABI_ID, first.generation, &original);
        check(result.action == OS64_DOM_RECOVERY_READY && !memcmp(&original, &saved, sizeof(saved)),
              "recovery host: valid generation recovers without changing original storage");
        realm_script(f.realm[2], "globalThis.bad=function(){shared.value=84;while(true){}}",
                     "recovery host: second independent episode");
        original = dom_recovery_overrun_realm(&f, 2);
        os64_js_recovery_info_t second;
        check(os64_js_recovery_inspect(f.runtime, OS64_JS_ABI_ID, &second) == OS64_JS_OK &&
              second.generation != first.generation, "recovery host: second episode advances generation");
        result = os64_dom_cluster_resolve_failure(f.cluster, OS64_JS_ABI_ID, first.generation, &original);
        check(result.action == OS64_DOM_RECOVERY_ROOT_REQUIRED && result.status == OS64_JS_ADMISSION_REFUSED &&
              !os64_js_realm_is_failed(f.realm[2]),
              "recovery host: previous generation cannot acknowledge a new child failure");
        result = os64_dom_cluster_resolve_failure(f.cluster, OS64_JS_ABI_ID, second.generation, &original);
        check(result.action == OS64_DOM_RECOVERY_READY && result.generation == second.generation,
              "recovery host: second generation has its own host acknowledgment");
        os64_js_cancel(f.runtime);
        result = os64_dom_cluster_resolve_failure(f.cluster, OS64_JS_ABI_ID, second.generation, &original);
        check(result.action == OS64_DOM_RECOVERY_ROOT_REQUIRED && result.status == OS64_JS_CANCELLED,
              "recovery host: cancellation defeats a repeated ready acknowledgment");
        dom_recovery_close(&f);
        if (!dom_recovery_open(&f, policy, true)) continue;
        f.provider[1]->store.forced = OS64_DOM_STORAGE_FAILED;
        original = dom_recovery_overrun(&f); saved = original;
        check(os64_js_recovery_inspect(f.runtime, OS64_JS_ABI_ID, &first) == OS64_JS_OK,
              "recovery host: native failure generation snapshot");
        result = os64_dom_cluster_resolve_failure(f.cluster, OS64_JS_ABI_ID, first.generation, &original);
        check(result.action == OS64_DOM_RECOVERY_ROOT_REQUIRED && result.status == OS64_JS_HOST_FAILURE &&
              result.host_error == OS64_DOM_STORAGE_FAILED && !memcmp(&original, &saved, sizeof(saved)),
              "recovery host: native settlement cause stays separate from original execution diagnostics");
        dom_recovery_close(&f);
    }
}
static void dom_recovery_host_cases(void)
{
    for (unsigned policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; ++policy)
    for (unsigned mode = 0; mode < 6; ++mode) {
        recovery_consumer_case(policy, mode, recovery_host_require, recovery_host_clock);
        check(!live && !dom_live && !dom_allocations, "recovery host: engine and native allocations balance");
    }
    recovery_host_edge_cases();
}
