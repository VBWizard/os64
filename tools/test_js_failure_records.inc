#include "os64/js_failure.h"

static os64_js_runtime_t *record_runtime;
static os64_js_failure_record_t record_inside;

static bool record_has(const os64_js_failure_record_t *record, os64_js_realm_t *realm)
{
    for (size_t i = 0; i < record->realm_count; i++)
        if (record->realms[i] == realm) return true;
    return false;
}
static JSValue record_function(JSContext *ctx, const char *source)
{
    JSValue fn = JS_Eval(ctx, source, strlen(source), "failure-record.js", JS_EVAL_TYPE_GLOBAL);
    check(JS_IsFunction(ctx, fn), "failure-record fixture compiles a callable");
    return fn;
}
static os64_js_runtime_t *record_create(unsigned policy, os64_js_realm_t **realm, JSContext **ctx)
{
    os64_js_config_t config = fixture_config();
    config.limits.execution_ms = 10;
    os64_js_outcome_t out;
    os64_js_runtime_t *runtime = NULL;
    check(os64_js_create_with_failure_records(&config, policy, OS64_JS_ABI_ID,
          &runtime, &out) == OS64_JS_OK, "failure-record runtime constructed");
    if (!runtime) return NULL;
    realm[0] = os64_js_primary_realm(runtime, OS64_JS_ABI_ID, &out);
    for (unsigned i = 1; i < 3; i++)
        check(os64_js_realm_create(runtime, OS64_JS_ABI_ID, &realm[i], &out) == OS64_JS_OK,
              "failure-record child created");
    for (unsigned i = 0; i < 3; i++)
        ctx[i] = os64_js_realm_context(realm[i], OS64_JS_ABI_ID, &out);
    check(ctx[0] && ctx[1] && ctx[2], "failure-record fixture exposes three contexts");
    return runtime;
}
static void record_close(os64_js_runtime_t *runtime, size_t before)
{
    clock_step = 0; clock_error = 0; fail_at = 0;
    os64_js_teardown_report_t report;
    os64_js_destroy_report(runtime, &report);
    check(!report.leaked && live == before, "failure records retain no roots after teardown");
}
static void record_observe(os64_js_runtime_t *runtime, os64_js_failure_record_t *record)
{
    size_t before = attempts;
    check(os64_js_observe_failure(runtime, OS64_JS_ABI_ID, record) == OS64_JS_OK,
          "failure observation succeeds without reopening runtime entry");
    check(attempts == before, "failure observation attempts no allocation");
}
static void record_original_same(const os64_js_failure_record_t *a, const os64_js_failure_record_t *b)
{
    check(a->original.status == b->original.status && a->original.limit == b->original.limit &&
          a->original.host_error == b->original.host_error && a->deadline == b->deadline &&
          a->jobs_executed == b->jobs_executed && a->jobs_discarded == b->jobs_discarded &&
          a->task_owner == b->task_owner && a->realm_count == b->realm_count &&
          a->scope_issues == b->scope_issues && a->scopes_classified == b->scopes_classified &&
          a->turn_open == b->turn_open && a->explicit_task == b->explicit_task,
          "late health observations preserve original verdict, deadline, counters and attribution");
    for (size_t i = 0; i < a->realm_count; i++)
        check(a->realms[i] == b->realms[i], "first-failure realm candidates remain stable after unwind");
}
static void record_call_timeout(os64_js_realm_t *owner,
                                os64_js_realm_t *entry, JSValue fn, os64_js_outcome_t *out)
{
    check(os64_js_realm_task_begin(owner, OS64_JS_ABI_ID, "record-task", out) == OS64_JS_OK,
          "failure-record task opens");
    clock_step = 1000;
    check(os64_js_realm_call(entry, OS64_JS_ABI_ID, fn, JS_UNDEFINED, 0, NULL, NULL, out) == OS64_JS_LIMIT &&
          out->limit == OS64_JS_LIMIT_EXECUTION, "child interruption retains original execution verdict");
    clock_step = 0;
}
static JSValue record_budget(JSContext *ctx, void *opaque)
{
    (void)ctx; (void)opaque;
    monotonic += 10000;
    size_t before = attempts;
    os64_js_outcome_t out;
    check(os64_js_check_budget(record_runtime, OS64_JS_ABI_ID, &out) == OS64_JS_LIMIT &&
          out.limit == OS64_JS_LIMIT_EXECUTION, "native budget observes timeout before scope returns");
    record_observe(record_runtime, &record_inside);
    check(attempts == before, "first native failure capture attempts no allocation");
    return JS_UNDEFINED;
}
static JSValue record_native_budget(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self; (void)argc; (void)argv;
    return record_budget(ctx, NULL);
}
static os64_js_status_t record_setup_budget(os64_js_realm_t *realm, JSContext *ctx, void *opaque)
{
    (void)realm;
    JSValue value = record_budget(ctx, opaque);
    JS_FreeValue(ctx, value);
    return OS64_JS_OK;
}
static JSValue record_job(JSContext *ctx, int argc, JSValueConst *argv)
{
    return argc ? JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL) : JS_UNDEFINED;
}
static void record_boundary_cases(unsigned policy)
{
    size_t before = live;
    os64_js_config_t config = fixture_config();
    config.limits.jobs_per_turn = 1;
    os64_js_runtime_t *runtime = NULL;
    os64_js_outcome_t out;
    os64_js_failure_record_t record, saved;
    check(os64_js_create_with_failure_records(&config, policy, OS64_JS_ABI_ID, &runtime, &out) == OS64_JS_OK,
          "job cap record fixture created");
    if (!runtime) return;
    os64_js_realm_t *child = NULL;
    check(os64_js_realm_create(runtime, OS64_JS_ABI_ID, &child, &out) == OS64_JS_OK,
          "job cap record child created");
    JSContext *ctx = os64_js_realm_context(child, OS64_JS_ABI_ID, &out);
    check(os64_js_realm_task_begin(child, OS64_JS_ABI_ID, "job-cap", &out) == OS64_JS_OK,
          "job cap record task opens");
    check(JS_EnqueueJobOwned(ctx, ctx, NULL, record_job, 0, NULL) == 0 &&
          JS_EnqueueJobOwned(ctx, NULL, NULL, record_job, 0, NULL) == 0,
          "child and neutral jobs share one cap");
    check(os64_js_checkpoint(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_LIMIT &&
          out.limit == OS64_JS_LIMIT_JOBS && out.jobs_executed == 1 && out.jobs_pending,
          "job cap stops before the neutral head executes");
    record_observe(runtime, &record);
    check(record.original.limit == OS64_JS_LIMIT_JOBS && record.global.limit == OS64_JS_LIMIT_JOBS &&
          record.jobs_executed == 1 && record.jobs_discarded == 0 && record.realm_count == 0 &&
          !record.job_execution && !record.job_callback && !record.job_activity,
          "cap exhaustion records original consumption without blaming the next queue head");
    check(os64_js_task_end(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_LIMIT &&
          out.jobs_executed == 1 && out.jobs_discarded == 0 && out.jobs_pending,
          "failed job task closes without draining or renewing its cap");
    record_close(runtime, before);

    config = fixture_config();
    check(os64_js_create_with_failure_records(&config, policy, OS64_JS_ABI_ID, &runtime, &out) == OS64_JS_OK,
          "non-limit and source record fixture created");
    if (!runtime) return;
    check(execute_fixture(runtime, "throw 7", &out) == OS64_JS_EXCEPTION,
          "ordinary exception remains an ordinary exception");
    record_observe(runtime, &record);
    check(record.original.status == OS64_JS_OK && record.global.status == OS64_JS_OK,
          "ordinary exception invents no terminal failure record");
    check(execute_fixture(runtime, "Promise.resolve().then(()=>{})", &out) == OS64_JS_OK && out.jobs_executed == 1,
          "completed earlier turn has a nonzero job count");
    saved = record;
    int64_t now = monotonic;
    clock_error = -83;
    check(os64_js_observe_failure(NULL, OS64_JS_ABI_ID, &record) == OS64_JS_BAD_ARGUMENT &&
          os64_js_observe_failure(runtime, NULL, &record) == OS64_JS_BAD_ARGUMENT &&
          os64_js_observe_failure(runtime, OS64_JS_ABI_ID, NULL) == OS64_JS_BAD_ARGUMENT &&
          os64_js_observe_failure(runtime, "wrong", &record) == OS64_JS_ABI_MISMATCH &&
          !memcmp(&record, &saved, sizeof(record)) && monotonic == now,
          "invalid observations preserve output and perform no health polling");
    record_observe(runtime, &record);
    check(record.original.status == OS64_JS_OK, "idle observation does not sample a new execution deadline");
    clock_error = 0;
    os64_js_cancel(runtime);
    record_observe(runtime, &record);
    check(record.original.status == OS64_JS_CANCELLED && record.global.status == OS64_JS_CANCELLED &&
          !record.turn_open && !record.task_owner && !record.deadline && !record.jobs_executed && !record.jobs_discarded,
          "failure outside a turn does not inherit an earlier task's deadline or counters");
    record_close(runtime, before);
    check(os64_js_create_with_failure_records(&config, policy, OS64_JS_ABI_ID, &runtime, &out) == OS64_JS_OK,
          "source record fixture created");
    if (!runtime) return;
    char *large = malloc(config.limits.source_bytes + 1);
    check(large != NULL, "oversized source fixture allocated");
    if (large) {
        memset(large, ' ', config.limits.source_bytes + 1);
        check(os64_js_run(runtime, large, config.limits.source_bytes + 1, "too-large", &out) == OS64_JS_LIMIT &&
              out.limit == OS64_JS_LIMIT_SOURCE, "source refusal retains terminal policy");
        free(large);
        record_observe(runtime, &record);
        check(record.original.limit == OS64_JS_LIMIT_SOURCE && record.global.limit == OS64_JS_LIMIT_SOURCE &&
              !record.turn_open && !record.task_owner && !record.deadline && !record.jobs_executed && !record.jobs_discarded,
              "source refusal has no invented task or counters");
    }
    record_close(runtime, before);

    config.limits.memory_bytes = 256 * 1024 * 1024;
    config.limits.execution_ms = 10;
    check(os64_js_create_with_failure_records(&config, policy, OS64_JS_ABI_ID, &runtime, &out) == OS64_JS_OK,
          "65-slot record fixture created");
    if (!runtime) return;
    child = NULL;
    for (unsigned i = 1; i < OS64_JS_MAX_REALMS; i++)
        check(os64_js_realm_create(runtime, OS64_JS_ABI_ID, &child, &out) == OS64_JS_OK,
              "65-slot record fixture admits its final child");
    ctx = os64_js_realm_context(child, OS64_JS_ABI_ID, &out);
    JSValue fn = record_function(ctx, "(function(){while(true){}})");
    record_call_timeout(child, child, fn, &out);
    record_observe(runtime, &record);
    check(record.scopes_classified && record.realm_count == 1 && record.realms[0] == child &&
          !(record.scope_issues & OS64_JS_FAILURE_SCOPE_OVERFLOW),
          "the 65th stable realm slot survives capture without a 64-bit mask");
    JS_FreeValue(ctx, fn);
    check(os64_js_task_end(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_LIMIT,
          "65-slot failure task closes");
    record_close(runtime, before);
}

static void failure_record_cases(void)
{
    size_t initial_live = live;
    os64_js_config_t config = fixture_config();
    os64_js_outcome_t out;
    os64_js_runtime_t *runtime = create_fixture(&config);
    os64_js_failure_record_t record, saved;
    memset(&record, 0x5a, sizeof(record)); saved = record;
    size_t allocations = attempts;
    check(os64_js_observe_failure(runtime, OS64_JS_ABI_ID, &record) == OS64_JS_ADMISSION_REFUSED &&
          !memcmp(&record, &saved, sizeof(record)) && attempts == allocations,
          "legacy runtime refuses diagnostic opt-in query without touching output");
    os64_js_destroy(runtime);
    runtime = (void *)1;
    check(os64_js_create_with_failure_records(&config, OS64_JS_TEARDOWN_FATAL, "wrong", &runtime, &out) ==
          OS64_JS_ABI_MISMATCH && runtime == NULL, "record constructor checks ABI before publication");
    fail_at = attempts + 1;
    check(os64_js_create_with_failure_records(&config, OS64_JS_TEARDOWN_RECLAIM, OS64_JS_ABI_ID,
          &runtime, &out) == OS64_JS_HOST_FAILURE && runtime == NULL,
          "record construction failure publishes no runtime");
    fail_at = 0;
    for (unsigned policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; policy++) {
        for (unsigned mode = 0; mode < 4; mode++) {
            os64_js_realm_t *realm[3] = {0}; JSContext *ctx[3] = {0};
            runtime = record_create(policy, realm, ctx);
            if (!runtime) continue;
            record_observe(runtime, &record);
            check(record.original.status == OS64_JS_OK && record.global.status == OS64_JS_OK &&
                  record.realm_count == 0 && !record.scopes_classified,
                  "healthy record contains no invented failure or completeness claim");
            check(os64_js_heap_headroom(runtime) + os64_js_heap_bytes(runtime) + os64_malloc_size(runtime) ==
                  config.limits.memory_bytes, "inline record storage is reserved in the configured ceiling");
            JSValue leaf = record_function(ctx[2], "(function(){while(true){}})");
            JSValue global = JS_GetGlobalObject(ctx[1]);
            check(JS_SetPropertyStr(ctx[1], global, "leaf", JS_DupValue(ctx[1], leaf)) == 1,
                  "foreign child function installed");
            JS_FreeValue(ctx[1], global);
            JSValue fn = mode == 0 ? record_function(ctx[1], "(function(){while(true){}})") :
                                     record_function(ctx[1], "(function(){leaf();return 1})");
            JSContext *foreign = NULL;
            if (mode == 2) {
                global = JS_GetGlobalObject(ctx[0]);
                JS_SetPropertyStr(ctx[0], global, "child", JS_DupValue(ctx[0], fn));
                JS_FreeValue(ctx[0], global); JS_FreeValue(ctx[1], fn);
                fn = record_function(ctx[0], "(function(){child();return 2})");
            }
            if (mode == 3) {
                foreign = JS_NewContext(JS_GetRuntime(ctx[0]));
                check(foreign != NULL, "foreign raw context created for fail-closed attribution");
                JS_SetContextOpaque(foreign, realm[1]);
                check(os64_js_realm_task_begin(realm[1], OS64_JS_ABI_ID, "foreign", &out) == OS64_JS_OK,
                      "foreign raw-entry task opens");
                clock_step = 1000;
                JSValue result = JS_Call(foreign, fn, JS_UNDEFINED, 0, NULL);
                check(JS_IsException(result), "foreign raw entry is interrupted");
                if (JS_IsException(result)) JS_FreeValue(foreign, JS_GetException(foreign));
                JS_FreeValue(foreign, result); clock_step = 0;
            } else {
                record_call_timeout(realm[1], realm[0], fn, &out);
            }
            record_observe(runtime, &record);
            check(record.original.status == OS64_JS_LIMIT && record.original.limit == OS64_JS_LIMIT_EXECUTION &&
                  record.global.status == OS64_JS_OK && record.turn_open && record.explicit_task &&
                  record.task_owner == realm[1] && record.deadline > 0,
                  "original task facts are independent of global health and host call context");
            check(record_has(&record, realm[1]) && record_has(&record, realm[2]) == (mode != 0) &&
                  record_has(&record, realm[0]) == (mode == 2) && record.realm_count == (mode == 0 ? 1u : mode == 2 ? 3u : 2u),
                  "capture includes abandoned child and primary code but excludes idle call contexts");
            check(record.scopes_classified == (mode != 3) &&
                  !!(record.scope_issues & OS64_JS_FAILURE_FOREIGN_CONTEXT) == (mode == 3),
                  "foreign context membership ignores spoofed opaque storage");
            saved = record;
            check(os64_js_task_end(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_LIMIT &&
                  out.limit == OS64_JS_LIMIT_EXECUTION, "failure-close preserves original outcome");
            record_observe(runtime, &record); record_original_same(&saved, &record);
            check(os64_js_realm_task_begin(realm[2], OS64_JS_ABI_ID, "sibling", &out) == OS64_JS_FAILED_RUNTIME &&
                  os64_js_set_execution_ms(runtime, 100, &out) == OS64_JS_FAILED_RUNTIME &&
                  os64_js_quiescent(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_FAILED_RUNTIME,
                  "observing records grants no sibling task, new budget or recovery safe point");
            JS_FreeValue(ctx[1], fn); JS_FreeValue(ctx[2], leaf);
            if (foreign) JS_FreeContext(foreign);
            record_close(runtime, initial_live);
        }
        for (unsigned mode = 0; mode < 5; mode++) {
            os64_js_realm_t *realm[3] = {0}; JSContext *ctx[3] = {0};
            runtime = record_create(policy, realm, ctx);
            if (!runtime) continue;
            record_runtime = runtime;
            JSValue fn = JS_NewCFunction(ctx[1], record_native_budget, "budget", 0);
            check(os64_js_realm_task_begin(realm[1], OS64_JS_ABI_ID, "native", &out) == OS64_JS_OK,
                  "native observation task opens");
            check(os64_js_realm_call(realm[1], OS64_JS_ABI_ID, fn, JS_UNDEFINED, 0, NULL, NULL, &out) == OS64_JS_LIMIT,
                  "native timeout reaches wrapper as terminal failure");
            record_observe(runtime, &saved);
            check(!saved.scopes_classified && (saved.scope_issues & OS64_JS_FAILURE_NATIVE_SCOPE) &&
                  saved.realm_count == 0 && record_inside.original.limit == OS64_JS_LIMIT_EXECUTION,
                  "native definition realm does not invent an abandoned author frame");
            if (mode == 0) os64_js_cancel(runtime);
            if (mode == 1) {
                void *refused = js_malloc_rt(JS_GetRuntime(ctx[1]), 32 * 1024 * 1024);
                check(refused == NULL, "late allocation limit injected");
                js_free_rt(JS_GetRuntime(ctx[1]), refused);
            }
            if (mode == 2) {
                fail_at = attempts + 1;
                void *refused = js_malloc_rt(JS_GetRuntime(ctx[1]), 64 * 1024);
                fail_at = 0;
                check(refused == NULL, "late host allocator refusal injected");
                js_free_rt(JS_GetRuntime(ctx[1]), refused);
            }
            if (mode == 3 || mode == 4) clock_error = -79;
            if (mode == 4) {
                check(os64_js_task_end(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_LIMIT,
                      "failed task close observes later clock failure");
            }
            record_observe(runtime, &record);
            check(record.global.status == (mode == 0 ? OS64_JS_CANCELLED : mode == 1 ? OS64_JS_LIMIT : OS64_JS_HOST_FAILURE) &&
                  record.global.limit == (mode == 1 ? OS64_JS_LIMIT_MEMORY : OS64_JS_LIMIT_NONE) &&
                  record.global.host_error == (mode >= 3 ? -79 : 0),
                  "later global cause is recorded after timeout and task closure");
            record_original_same(&saved, &record); saved = record;
            os64_js_cancel(runtime); clock_error = -81;
            record_observe(runtime, &record);
            check(record.global.status == saved.global.status && record.global.limit == saved.global.limit &&
                  record.global.host_error == saved.global.host_error, "first global cause remains sticky");
            clock_error = 0;
            JSValue marker = JS_NewError(ctx[1]);
            JS_Throw(ctx[1], JS_DupValue(ctx[1], marker));
            record_observe(runtime, &record);
            JSValue pending = JS_GetException(ctx[1]);
            check(JS_VALUE_GET_PTR(pending) == JS_VALUE_GET_PTR(marker), "record observation preserves pending exception identity");
            JS_FreeValue(ctx[1], pending); JS_FreeValue(ctx[1], marker);
            if (mode != 4) check(os64_js_task_end(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_LIMIT &&
                                 out.limit == OS64_JS_LIMIT_EXECUTION, "later global cause does not rewrite task end verdict");
            JS_FreeValue(ctx[1], fn); record_close(runtime, initial_live);
        }
        for (unsigned mode = 0; mode < 4; mode++) {
            os64_js_realm_t *realm[3] = {0}; JSContext *ctx[3] = {0};
            runtime = record_create(policy, realm, ctx);
            if (!runtime) continue;
            record_runtime = runtime;
            if (mode == 0) {
                clock_step = 1000;
                check(realm_execute(realm[1], "while(true){}", &out) == OS64_JS_LIMIT,
                      "implicit eval timeout recorded");
                clock_step = 0;
            } else {
                check(os64_js_realm_task_begin(realm[1], OS64_JS_ABI_ID, "scope", &out) == OS64_JS_OK,
                      "native scope observation task opens");
                if (mode == 1) {
                    JSValue value = JS_UNDEFINED;
                    check(os64_js_realm_with_settings(realm[1], NULL, OS64_JS_ABI_ID,
                          record_budget, NULL, &value, &out) == OS64_JS_LIMIT,
                          "settings body timeout recorded before native unwind");
                    JS_FreeValue(ctx[1], value);
                } else {
                    JSValue fn = record_function(ctx[1], "(function(){while(true){}})");
                    check(JS_EnqueueJobOwned(ctx[1], realm[2] ? ctx[2] : NULL, ctx[0], record_job, 1, &fn) == 0,
                          "mixed-provenance job queued");
                    clock_step = 1000;
                    if (mode == 2) check(os64_js_checkpoint(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_LIMIT,
                                         "wrapper job timeout recorded");
                    else {
                        JSContext *job_ctx;
                        check(JS_ExecutePendingJob(JS_GetRuntime(ctx[0]), &job_ctx) < 0,
                              "raw job timeout recorded without wrapper provenance");
                        JS_FreeValue(job_ctx, JS_GetException(job_ctx));
                    }
                    clock_step = 0; JS_FreeValue(ctx[1], fn);
                }
            }
            record_observe(runtime, &record);
            check(!record.scopes_classified, "eval, native settings and job scopes do not certify completeness");
            if (mode == 0) check(!record.explicit_task && (record.scope_issues & OS64_JS_FAILURE_UNKNOWN_CODE) &&
                                 record_has(&record, realm[1]), "eval preparation remains explicitly unknown");
            if (mode == 1) check((record.scope_issues & OS64_JS_FAILURE_SETTINGS_SCOPE) &&
                                 (record.scope_issues & OS64_JS_FAILURE_NO_SCOPE), "native settings body is visible without engine frames");
            if (mode >= 2) check((record.scope_issues & OS64_JS_FAILURE_JOB_SCOPE) &&
                                  record.job_execution == (mode == 2 ? realm[1] : NULL) &&
                                  record.job_callback == (mode == 2 ? realm[2] : NULL) &&
                                  record.job_activity == (mode == 2 ? realm[0] : NULL) &&
                                  record.realm_count == 1 && record_has(&record, realm[1]),
                                  "job code, execution, callback and activity remain distinct facts");
            if (mode != 0) check(os64_js_task_end(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_LIMIT,
                                 "scope fixture closes failed task");
            record_close(runtime, initial_live);
        }
        os64_js_realm_t *realm[3] = {0}; JSContext *ctx[3] = {0};
        runtime = record_create(policy, realm, ctx);
        if (!runtime) continue;
        record_runtime = runtime;
        os64_js_realm_t *staged = NULL;
        check(os64_js_realm_stage(runtime, OS64_JS_ABI_ID, 1, &staged, &out) == OS64_JS_OK,
              "candidate staged for native setup observation");
        check(os64_js_realm_setup(staged, OS64_JS_ABI_ID, record_setup_budget, NULL, &out) == OS64_JS_LIMIT,
              "candidate setup timeout retains global fallback");
        record_observe(runtime, &record);
        check((record.scope_issues & OS64_JS_FAILURE_SETUP_SCOPE) && !record.scopes_classified,
              "candidate transaction is recorded before setup unwinds");
        record_close(runtime, initial_live);
        record_boundary_cases(policy);
    }
    check(live == initial_live, "failure-record matrix releases all native and engine allocations");
}
