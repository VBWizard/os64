#include "os64/js_callbacks.h"

typedef struct {
    os64_js_runtime_t *runtime;
    os64_js_realm_t *realm[4];
    JSContext *ctx[4];
    os64_js_callback_t callback[4];
    unsigned calls, cleaned, mode, loans;
    void *provider;
    bool provider_released;
    unsigned queue[4], queue_count;
    JSValue capture_value;
} RecoveryHarness;
static unsigned recovery_job_hits;
static JSValue recovery_pending_job(JSContext *ctx, int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    recovery_job_hits++;
    return JS_UNDEFINED;
}

static os64_js_status_t recovery_host_prepare(void *opaque, const os64_js_recovery_token_t *token,
                                      const os64_js_failure_record_t *failure, int64_t *host_error)
{
    RecoveryHarness *h = opaque;
    h->calls++;
    os64_js_recovery_info_t info;
    check(token->runtime == h->runtime && os64_js_recovery_inspect(h->runtime, OS64_JS_ABI_ID, &info) == OS64_JS_OK &&
          info.state == OS64_JS_RECOVERY_PREPARING && info.generation == token->generation,
          "native preparation observes its exact pending generation under guard");
    JSExecutionState state;
    check(JS_GetExecutionState(JS_GetRuntime(h->ctx[0]), &state) && state.blocked,
          "native preparation runs behind the engine execution fence");
    os64_js_outcome_t out;
    os64_js_recovery_token_t untouched = {NULL, 91};
    check(os64_js_recovery_commit(h->runtime, OS64_JS_ABI_ID, token, &out) == OS64_JS_BUSY &&
          os64_js_recovery_prepare(h->runtime, OS64_JS_ABI_ID, &untouched, &out) == OS64_JS_BUSY && untouched.generation == 91,
          "native callback cannot recursively prepare or commit its generation");
    check(realm_execute(h->realm[0], "throw 7", &out) == OS64_JS_BUSY,
          "native preparation cannot enter a wrapper script");
    JSValue result = JS_Eval(h->ctx[0], "globalThis.escaped=1", 20, "recovery-cleanup", JS_EVAL_TYPE_GLOBAL);
    check(JS_IsException(result) && !JS_HasException(h->ctx[0]),
          "native preparation cannot enter raw healthy code or replace the aborted exception");
    JS_FreeValue(h->ctx[0], result);
    if (h->loans) {
        check(h->provider != NULL && !h->provider_released,
              "busy native preparation retains provider storage while loans remain");
        return OS64_JS_BUSY;
    }
    for (size_t i = 0; i < failure->realm_count; i++) {
        os64_js_realm_t *realm = failure->realms[i];
        check(os64_js_realm_is_failed(realm) && os64_js_realm_is_retired(realm),
              "executable and activity ineligibility publish before native cleanup");
        for (unsigned j = 0; j < 4; j++) if (h->realm[j] == realm) {
            h->cleaned |= 1u << j;
            check(JS_IsContextExecutableFailed(h->ctx[j]), "engine failed-code guard publishes before native preparation");
            check(os64_js_callback_clear(h->runtime, OS64_JS_ABI_ID, &h->callback[j], &out) == OS64_JS_OK,
                  "native failed callback root releases inside the recovery fence");
        }
    }
    unsigned kept = 0;
    for (unsigned i = 0; i < h->queue_count; i++)
        if (!(h->cleaned & (1u << h->queue[i]))) h->queue[kept++] = h->queue[i];
    h->queue_count = kept;
    if (h->mode == 1) { *host_error = -71; return OS64_JS_HOST_FAILURE; }
    if (h->mode == 2) os64_js_cancel(h->runtime);
    if (h->mode == 3) clock_error = -72;
    if (h->mode == 4) {
        fail_at = attempts + 1;
        void *p = js_malloc_rt(JS_GetRuntime(h->ctx[0]), 64 * 1024);
        fail_at = 0;
        check(p == NULL, "native cleanup OOM is injected through the real host allocator");
        js_free_rt(JS_GetRuntime(h->ctx[0]), p);
    }
    if (h->mode == 5) {
        void *p = js_malloc_rt(JS_GetRuntime(h->ctx[0]), 32 * 1024 * 1024);
        check(p == NULL, "native cleanup memory limit is injected");
        js_free_rt(JS_GetRuntime(h->ctx[0]), p);
    }
    if (h->mode == 6) check(JS_EnqueueJob(h->ctx[0], recovery_pending_job, 0, NULL) == 0,
                            "native cleanup queues work for conservative commit refusal");
    if (h->mode == 7) JS_ThrowInternalError(h->ctx[0], "native cleanup failed");
    if (h->mode == 8) monotonic += 1000000;
    if (h->provider) { os64_free(h->provider); h->provider = NULL; h->provider_released = true; }
    return OS64_JS_OK;
}
static JSValue recovery_capture_body(JSContext *ctx, void *opaque)
{
    RecoveryHarness *h = opaque;
    os64_js_outcome_t out;
    for (unsigned i = 0; i < 4; i++) if (h->ctx[i] == ctx) {
        check(os64_js_callback_capture(h->runtime, OS64_JS_ABI_ID, h->capture_value, &h->callback[i], &out) == OS64_JS_OK,
              "native timer fixture captures the conversion-time incumbent");
    }
    return JS_UNDEFINED;
}
static JSValue recovery_callback_call(JSContext *ctx, JSValueConst function, void *opaque)
{
    (void)opaque;
    return JS_Call(ctx, function, JS_UNDEFINED, 0, NULL);
}
static bool recovery_fixture(RecoveryHarness *h, unsigned policy)
{
    os64_memset(h, 0, sizeof(*h));
    for (unsigned i = 0; i < 4; i++) h->callback[i] = (os64_js_callback_t)OS64_JS_CALLBACK_INIT;
    os64_js_config_t config = fixture_config(); config.limits.execution_ms = 10;
    os64_js_recovery_host_t host = {.opaque=h, .prepare=recovery_host_prepare};
    os64_js_outcome_t out;
    check(os64_js_create_with_child_recovery(&config, policy, OS64_JS_ABI_ID, &host, &h->runtime, &out) == OS64_JS_OK,
          "explicit native-certified recovery runtime constructed");
    if (!h->runtime) return false;
    h->realm[0] = os64_js_primary_realm(h->runtime, OS64_JS_ABI_ID, &out);
    for (unsigned i = 1; i < 4; i++)
        check(os64_js_realm_create(h->runtime, OS64_JS_ABI_ID, &h->realm[i], &out) == OS64_JS_OK,
              "recovery fixture child created with retained slot accounting");
    h->provider = os64_malloc(32);
    check(h->provider != NULL, "native provider fixture allocated");
    if (h->provider) memset(h->provider, 0x5a, 32);
    for (unsigned i = 0; i < 4; i++) h->ctx[i] = os64_js_realm_context(h->realm[i], OS64_JS_ABI_ID, &out);
    recovery_job_hits = 0;
    execution_eval(h->ctx[1], "globalThis.box={value:41,order:0};globalThis.bad=function(probe){"
                               "if(probe){box.forbidden=1;return 7}box.value=42;"
                               "try{while(true){}}finally{box.value=99}};");
    for (unsigned i = 0; i < 4; i++) if (i != 1) {
        realm_share(h->ctx[1], h->ctx[i], "box"); realm_share(h->ctx[1], h->ctx[i], "bad");
    }
    for (unsigned i = 0; i < 4; i++) {
        char source[96]; snprintf(source, sizeof(source), "(function(){box.order=box.order*10+%u})", i + 1);
        h->capture_value = record_function(h->ctx[i], source);
        check(os64_js_realm_task_begin(h->realm[i], OS64_JS_ABI_ID, "capture", &out) == OS64_JS_OK,
              "native callback capture task opens");
        JSValue value = JS_UNDEFINED;
        check(os64_js_realm_with_settings(h->realm[i], h->realm[i], OS64_JS_ABI_ID,
              recovery_capture_body, h, &value, &out) == OS64_JS_OK, "native callback captured under checked incumbent");
        JS_FreeValue(h->ctx[i], value); JS_FreeValue(h->ctx[i], h->capture_value);
        check(os64_js_task_end(h->runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK, "capture task closes without pending jobs");
    }
    h->queue[0] = 2; h->queue[1] = 1; h->queue[2] = 0; h->queue_count = 3;
    return true;
}
static void recovery_destroy(RecoveryHarness *h, size_t before)
{
    clock_error = 0; clock_step = 0; fail_at = 0;
    os64_js_outcome_t out;
    for (unsigned i = 0; i < 4; i++)
        check(os64_js_callback_clear(h->runtime, OS64_JS_ABI_ID, &h->callback[i], &out) == OS64_JS_OK,
              "native callback roots clear during final/global fallback cleanup");
    if (h->provider) {
        check(h->loans == 0, "root cleanup waits for native provider loans too");
        os64_free(h->provider); h->provider = NULL;
    }
    os64_js_teardown_report_t report;
    os64_js_destroy_report(h->runtime, &report);
    check(!report.leaked && live == before, "recovery and fallback release native/engine roots under selected teardown");
}
static os64_js_outcome_t recovery_overrun(RecoveryHarness *h, unsigned child, bool implicit)
{
    os64_js_outcome_t out;
    if (implicit) {
        clock_step = 1000;
        check(realm_execute(h->realm[child], "box.value=42;try{while(true){}}finally{box.value=99}", &out) == OS64_JS_LIMIT &&
              out.limit == OS64_JS_LIMIT_EXECUTION, "implicit child turn reports its original timeout");
        clock_step = 0;
    } else {
        JSValue fn = realm_property(h->ctx[child], "bad");
        check(os64_js_realm_task_begin(h->realm[child], OS64_JS_ABI_ID, "child-overrun", &out) == OS64_JS_OK,
              "child overrun task opens");
        if (h->mode == 9) {
            check(JS_EnqueueJob(h->ctx[2], recovery_pending_job, 0, NULL) == 0 &&
                  os64_js_checkpoint(h->runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK && out.jobs_executed == 1,
                  "pre-overrun checkpoint charges the original shared task");
        }
        clock_step = 1000;
        check(os64_js_realm_call(h->realm[0], OS64_JS_ABI_ID, fn, JS_UNDEFINED, 0, NULL, NULL, &out) == OS64_JS_LIMIT &&
              out.limit == OS64_JS_LIMIT_EXECUTION, "child interruption retains original execution verdict");
        clock_step = 0;
        JS_FreeValue(h->ctx[child], fn);
    }
    return out;
}
static void recovery_inject(RecoveryHarness *h, unsigned mode)
{
    if (mode == 2) os64_js_cancel(h->runtime);
    if (mode == 3) clock_error = -72;
    if (mode == 4) {
        fail_at = attempts + 1;
        void *p = js_malloc_rt(JS_GetRuntime(h->ctx[0]), 64 * 1024);
        fail_at = 0;
        check(p == NULL, "late host OOM injected"); js_free_rt(JS_GetRuntime(h->ctx[0]), p);
    }
    if (mode == 5) {
        void *p = js_malloc_rt(JS_GetRuntime(h->ctx[0]), 32 * 1024 * 1024);
        check(p == NULL, "late memory refusal injected"); js_free_rt(JS_GetRuntime(h->ctx[0]), p);
    }
    if (mode == 6) check(JS_EnqueueJob(h->ctx[2], recovery_pending_job, 0, NULL) == 0,
                        "healthy engine job queued without permission to renew its budget");
    if (mode == 7) JS_ThrowInternalError(h->ctx[0], "pending native failure");
}
static void recovery_health_cases(unsigned policy, size_t before)
{
    for (unsigned phase = 0; phase < 3; phase++) {
        for (unsigned mode = phase == 1 ? 1 : 2; mode <= 7; mode++) {
            RecoveryHarness h;
            if (!recovery_fixture(&h, policy)) continue;
            os64_js_outcome_t original = recovery_overrun(&h, 1, false), out;
            check(os64_js_task_end(h.runtime, OS64_JS_ABI_ID, &out) == OS64_JS_LIMIT,
                  "late-health fixture closes original failed task");
            os64_js_recovery_info_t first, current;
            check(os64_js_recovery_inspect(h.runtime, OS64_JS_ABI_ID, &first) == OS64_JS_OK,
                  "late-health fixture snapshots original failure");
            os64_js_recovery_token_t token = {NULL,95};
            os64_js_status_t result;
            if (phase == 0) {
                recovery_inject(&h, mode);
                result = os64_js_recovery_prepare(h.runtime, OS64_JS_ABI_ID, &token, &out);
            } else if (phase == 1) {
                h.mode = mode;
                result = os64_js_recovery_prepare(h.runtime, OS64_JS_ABI_ID, &token, &out);
            } else {
                check(os64_js_recovery_prepare(h.runtime, OS64_JS_ABI_ID, &token, &out) == OS64_JS_OK,
                      "commit health fixture prepares native containment");
                recovery_inject(&h, mode);
                result = os64_js_recovery_commit(h.runtime, OS64_JS_ABI_ID, &token, &out);
            }
            check(result != OS64_JS_OK && h.calls == (phase == 0 ? 0u : 1u),
                  "late global/queued/exceptional work prevents recovery at its exact boundary");
            if (phase < 2) check(token.runtime == NULL && token.generation == 95,
                                 "failed preparation does not publish a usable generation token");
            check(os64_js_recovery_inspect(h.runtime, OS64_JS_ABI_ID, &current) == OS64_JS_OK &&
                  current.state == OS64_JS_RECOVERY_GLOBAL && current.generation == first.generation,
                  "global fallback cannot be acknowledged as a prepared or committed generation");
            record_original_same(&first.failure, &current.failure);
            check(original.status == OS64_JS_LIMIT && original.limit == OS64_JS_LIMIT_EXECUTION &&
                  current.jobs_executed == original.jobs_executed && !recovery_job_hits,
                  "global escalation preserves failed task verdict/counts and executes no healthy pending job");
            if (mode <= 5) {
                os64_js_status_t expected = mode == 2 ? OS64_JS_CANCELLED : mode == 5 ? OS64_JS_LIMIT : OS64_JS_HOST_FAILURE;
                check(current.failure.global.status == expected &&
                      current.failure.global.limit == (mode == 5 ? OS64_JS_LIMIT_MEMORY : OS64_JS_LIMIT_NONE) &&
                      current.failure.global.host_error == (mode == 1 ? -71 : mode == 3 ? -72 : 0),
                      "late global cause and available host code remain explicit");
            }
            check(os64_js_realm_task_begin(h.realm[2], OS64_JS_ABI_ID, "forbidden-sibling", &out) == OS64_JS_FAILED_RUNTIME &&
                  os64_js_set_execution_ms(h.runtime, 1000, &out) == OS64_JS_FAILED_RUNTIME,
                  "global fallback cannot be evaded with a sibling or replacement deadline");
            if (phase) {
                JSExecutionState state;
                check(JS_GetExecutionState(JS_GetRuntime(h.ctx[0]), &state) && state.blocked,
                      "failed native preparation or commit remains fenced until root cleanup");
                JSValue value = JS_Call(h.ctx[0], h.callback[0].value, JS_UNDEFINED, 0, NULL);
                check(JS_IsException(value), "raw healthy entry remains blocked after failed recovery");
                JS_FreeValue(h.ctx[0], value);
            }
            recovery_destroy(&h, before);
            check(!recovery_job_hits, "global root destruction discards pending jobs without executing bodies");
        }
    }
}
static void recovery_busy_and_generations(unsigned policy, size_t before)
{
    RecoveryHarness h;
    if (!recovery_fixture(&h, policy)) return;
    os64_js_outcome_t out;
    recovery_overrun(&h, 1, false);
    check(os64_js_task_end(h.runtime, OS64_JS_ABI_ID, &out) == OS64_JS_LIMIT, "loan fixture closes abandoned task");
    h.loans = 1;
    os64_js_recovery_token_t token = {NULL, 55};
    check(os64_js_recovery_prepare(h.runtime, OS64_JS_ABI_ID, &token, &out) == OS64_JS_BUSY &&
          token.runtime == NULL && token.generation == 55 && h.calls == 1 && !h.cleaned &&
          h.provider != NULL && !h.provider_released && ((unsigned char *)h.provider)[0] == 0x5a,
          "native BUSY retains provider opaque storage and withholds the prepared token");
    os64_js_recovery_info_t info;
    check(os64_js_recovery_inspect(h.runtime, OS64_JS_ABI_ID, &info) == OS64_JS_OK &&
          info.state == OS64_JS_RECOVERY_WAITING && os64_js_realm_is_failed(h.realm[1]),
          "waiting native loan remains behind the same irreversible child containment");
    os64_js_recovery_token_t guessed = {h.runtime, info.generation};
    check(os64_js_recovery_commit(h.runtime, OS64_JS_ABI_ID, &guessed, &out) == OS64_JS_ADMISSION_REFUSED,
          "native waiting generation cannot skip its completion certificate");
    h.loans = 0;
    check(os64_js_recovery_prepare(h.runtime, OS64_JS_ABI_ID, &token, &out) == OS64_JS_OK && h.calls == 2 &&
          token.generation == info.generation && h.provider_released && !h.provider &&
          os64_js_recovery_commit(h.runtime, OS64_JS_ABI_ID, &token, &out) == OS64_JS_OK,
          "loan release permits native completion and commit of the original generation");
    check(realm_execute(h.realm[2], "Promise.resolve().then(()=>{box.value=44})", &out) == OS64_JS_OK && out.jobs_executed == 1,
          "later independent sibling task owns its own executed job");
    check(os64_js_recovery_inspect(h.runtime, OS64_JS_ABI_ID, &info) == OS64_JS_OK &&
          info.state == OS64_JS_RECOVERY_COMMITTED && info.jobs_executed == 0 && info.failure.jobs_executed == 0,
          "completed recovery accounting does not adopt a later sibling task's job count");
    os64_js_recovery_token_t old = token;
    /* The second overrun runs B's loop body compiled in C so B stays sealed. */
    execution_eval(h.ctx[2], "globalThis.bad=function(probe){if(probe)return 7;while(true){}};");
    recovery_overrun(&h, 2, false);
    check(os64_js_task_end(h.runtime, OS64_JS_ABI_ID, &out) == OS64_JS_LIMIT, "second abandoned task closes");
    check(os64_js_recovery_inspect(h.runtime, OS64_JS_ABI_ID, &info) == OS64_JS_OK && info.generation > old.generation,
          "a later failure episode receives a distinct monotonically advancing generation");
    check(os64_js_recovery_commit(h.runtime, OS64_JS_ABI_ID, &old, &out) == OS64_JS_ADMISSION_REFUSED,
          "stale successful token cannot acknowledge a later failure");
    h.mode = 8;
    check(os64_js_recovery_prepare(h.runtime, OS64_JS_ABI_ID, &token, &out) == OS64_JS_OK &&
          os64_js_recovery_commit(h.runtime, OS64_JS_ABI_ID, &old, &out) == OS64_JS_ADMISSION_REFUSED &&
          os64_js_recovery_commit(h.runtime, OS64_JS_ABI_ID, &token, &out) == OS64_JS_OK,
          "native cleanup time cannot renew the abandoned budget or validate an old token");
    check(os64_js_realm_is_failed(h.realm[1]) && os64_js_realm_is_failed(h.realm[2]) &&
          realm_execute(h.realm[3], "box.value=45", &out) == OS64_JS_OK,
          "repeated child containment retains failed slots while a later independent child stays healthy");
    recovery_destroy(&h, before);

    RecoveryHarness a, b;
    if (!recovery_fixture(&a, policy)) return;
    if (!recovery_fixture(&b, policy)) { recovery_destroy(&a, before); return; }
    recovery_overrun(&a, 1, false); os64_js_task_end(a.runtime, OS64_JS_ABI_ID, &out);
    recovery_overrun(&b, 1, false); os64_js_task_end(b.runtime, OS64_JS_ABI_ID, &out);
    os64_js_recovery_token_t at, bt;
    check(os64_js_recovery_prepare(a.runtime, OS64_JS_ABI_ID, &at, &out) == OS64_JS_OK &&
          os64_js_recovery_prepare(b.runtime, OS64_JS_ABI_ID, &bt, &out) == OS64_JS_OK && at.generation == bt.generation,
          "separate runtimes can prepare equal-numbered generations independently");
    check(os64_js_recovery_commit(b.runtime, OS64_JS_ABI_ID, &at, &out) == OS64_JS_ADMISSION_REFUSED &&
          os64_js_recovery_commit(b.runtime, OS64_JS_ABI_ID, &bt, &out) == OS64_JS_OK &&
          os64_js_recovery_commit(a.runtime, OS64_JS_ABI_ID, &at, &out) == OS64_JS_OK,
          "runtime identity prevents cross-runtime prepared-token reuse");
    /* Both preparations changed A's live roots; use the joint allocation
     * baseline after both runtimes have been destroyed. */
    os64_js_teardown_report_t b_report;
    for (unsigned i = 0; i < 4; i++) os64_js_callback_clear(b.runtime, OS64_JS_ABI_ID, &b.callback[i], &out);
    if (b.provider) { os64_free(b.provider); b.provider = NULL; }
    os64_js_destroy_report(b.runtime, &b_report);
    check(!b_report.leaked,
          "foreign-token fixture releases B without retaining its resources");
    recovery_destroy(&a, before);
}

static JSValue recovery_exotic_budget(JSContext *ctx, JSValueConst obj, JSAtom name,
                                     JSValueConst receiver)
{
    (void)obj; (void)name; (void)receiver;
    return record_budget(ctx, NULL);
}
static JSClassID recovery_exotic_id;
static JSClassExoticMethods recovery_exotic_methods = {.get_property=recovery_exotic_budget};

static JSValue recovery_raw_settings_call(JSContext *ctx, void *opaque)
{
    JSValue *fn = opaque;
    return JS_Call(ctx, *fn, JS_UNDEFINED, 0, NULL);
}
static void recovery_scope_cases(unsigned policy, size_t before)
{
    for (unsigned mode = 0; mode < 15; mode++) {
        RecoveryHarness h;
        if (!recovery_fixture(&h, policy)) continue;
        record_runtime = h.runtime;
        os64_js_outcome_t out;
        unsigned issue = 0;
        JSContext *foreign = NULL;
        os64_js_module_t *pending_module = NULL;
        if (mode == 0 || mode == 1) {
            execution_eval(h.ctx[2], "globalThis.leaf=function(){while(true){}};");
            realm_share(h.ctx[2], h.ctx[1], "leaf");
            JSValue fn = record_function(h.ctx[1], "(function(){leaf();return 1})");
            if (mode == 1) {
                JSValue global = JS_GetGlobalObject(h.ctx[0]);
                JS_SetPropertyStr(h.ctx[0], global, "child", JS_DupValue(h.ctx[0], fn));
                JS_FreeValue(h.ctx[0], global); JS_FreeValue(h.ctx[1], fn);
                fn = record_function(h.ctx[0], "(function(){child();return 2})");
            }
            record_call_timeout(h.realm[1], h.realm[0], fn, &out);
            JS_FreeValue(h.ctx[0], fn);
        } else if (mode == 2) {
            check(os64_js_realm_task_begin(h.realm[1], OS64_JS_ABI_ID, "settings", &out) == OS64_JS_OK,
                  "unsupported settings task opens");
            JSValue value = JS_UNDEFINED;
            check(os64_js_realm_with_settings(h.realm[1], NULL, OS64_JS_ABI_ID,
                  record_budget, NULL, &value, &out) == OS64_JS_LIMIT, "native settings failure observed");
            JS_FreeValue(h.ctx[1], value); issue = OS64_JS_FAILURE_SETTINGS_SCOPE;
        } else if (mode == 3) {
            JSValue fn = JS_NewCFunction(h.ctx[1], record_native_budget, "nativeBudget", 0);
            check(os64_js_realm_task_begin(h.realm[1], OS64_JS_ABI_ID, "native", &out) == OS64_JS_OK,
                  "unsupported native task opens");
            check(os64_js_realm_call(h.realm[1], OS64_JS_ABI_ID, fn, JS_UNDEFINED, 0, NULL, NULL, &out) == OS64_JS_LIMIT,
                  "native failure remains terminal");
            JS_FreeValue(h.ctx[1], fn); issue = OS64_JS_FAILURE_NATIVE_SCOPE;
        } else if (mode == 4) {
            JSValue fn = realm_property(h.ctx[1], "bad");
            check(os64_js_realm_task_begin(h.realm[1], OS64_JS_ABI_ID, "job", &out) == OS64_JS_OK,
                  "unsupported job task opens");
            check(JS_EnqueueJob(h.ctx[1], record_job, 1, &fn) == 0, "unsupported job queued");
            JS_FreeValue(h.ctx[1], fn);
            clock_step = 1000;
            check(os64_js_checkpoint(h.runtime, OS64_JS_ABI_ID, &out) == OS64_JS_LIMIT,
                  "job timeout remains terminal");
            clock_step = 0; issue = OS64_JS_FAILURE_JOB_SCOPE;
        } else if (mode == 5) {
            os64_js_realm_t *staged = NULL;
            check(os64_js_realm_stage(h.runtime, OS64_JS_ABI_ID, 1, &staged, &out) == OS64_JS_OK,
                  "unsupported staged realm admitted");
            check(os64_js_realm_setup(staged, OS64_JS_ABI_ID, record_setup_budget, NULL, &out) == OS64_JS_LIMIT,
                  "native setup timeout remains terminal");
            issue = OS64_JS_FAILURE_SETUP_SCOPE;
        } else if (mode == 6) {
            execution_eval(h.ctx[1], "globalThis.gen=(function*(){while(true){}})();globalThis.resume=function(){gen.next()};");
            JSValue fn = realm_property(h.ctx[1], "resume");
            record_call_timeout(h.realm[1], h.realm[0], fn, &out);
            JS_FreeValue(h.ctx[1], fn); issue = OS64_JS_FAILURE_RESUME_SCOPE;
        } else if (mode == 7) {
            os64_js_module_t *module = NULL;
            static const char source[] = "while(true){};export const n=1;";
            check(os64_js_realm_module_compile(h.realm[1], OS64_JS_ABI_ID, source, sizeof(source)-1,
                  "recovery-module.mjs", "recovery-module.mjs", &module, &out) == OS64_JS_OK, "unsupported module compiled");
            check(os64_js_realm_task_begin(h.realm[1], OS64_JS_ABI_ID, "module", &out) == OS64_JS_OK,
                  "unsupported module task opens");
            clock_step = 1000;
            os64_js_status_t module_result = os64_js_realm_module_run(h.realm[1], OS64_JS_ABI_ID, module, &out);
            check(module_result == OS64_JS_LIMIT, "module timeout remains terminal");
            clock_step = 0; os64_js_realm_module_release(h.realm[1], OS64_JS_ABI_ID, module, &out);
            issue = OS64_JS_FAILURE_MODULE_SCOPE;
        } else if (mode == 8) {
            foreign = JS_NewContext(JS_GetRuntime(h.ctx[0]));
            JS_SetContextOpaque(foreign, h.realm[1]);
            JSValue fn = realm_property(h.ctx[1], "bad");
            check(os64_js_realm_task_begin(h.realm[1], OS64_JS_ABI_ID, "foreign", &out) == OS64_JS_OK,
                  "unsupported foreign task opens");
            clock_step = 1000;
            JSValue value = JS_Call(foreign, fn, JS_UNDEFINED, 0, NULL);
            check(JS_IsException(value), "foreign-context interruption observed");
            JS_FreeValue(foreign, JS_GetException(foreign)); JS_FreeValue(foreign, value);
            clock_step = 0; JS_FreeValue(h.ctx[1], fn); issue = OS64_JS_FAILURE_FOREIGN_CONTEXT;
        } else if (mode == 9) {
            check(os64_js_realm_task_begin(h.realm[1], OS64_JS_ABI_ID, "no-frame", &out) == OS64_JS_OK,
                  "unsupported frame-free task opens");
            monotonic += 10000;
            os64_js_failure_record_t no_scope;
            check(os64_js_observe_failure(h.runtime, OS64_JS_ABI_ID, &no_scope) == OS64_JS_OK &&
                  no_scope.original.status == OS64_JS_LIMIT, "no-scope timeout cannot invent author attribution");
            issue = OS64_JS_FAILURE_NO_SCOPE;
        } else if (mode == 10) {
            execution_eval(h.ctx[1], "Promise.reject(7)");
            recovery_overrun(&h, 1, false);
        } else if (mode == 11) {
            check(os64_js_realm_task_begin(h.realm[1], OS64_JS_ABI_ID, "cap", &out) == OS64_JS_OK,
                  "job-cap task opens");
            for (unsigned i = 0; i < 101; i++)
                check(JS_EnqueueJob(h.ctx[2], recovery_pending_job, 0, NULL) == 0, "healthy sibling cap job queued");
            check(os64_js_checkpoint(h.runtime, OS64_JS_ABI_ID, &out) == OS64_JS_LIMIT &&
                  out.limit == OS64_JS_LIMIT_JOBS && out.jobs_executed == 100 && out.jobs_pending && recovery_job_hits == 100,
                  "shared job cap never attributes exhaustion to the healthy next job");
        } else if (mode == 12) {
            static const char source[] = "export const x=1";
            check(os64_js_realm_module_compile(h.realm[3], OS64_JS_ABI_ID, source, sizeof(source)-1,
                  "pending.mjs", "pending.mjs", &pending_module, &out) == OS64_JS_OK,
                  "healthy module handle retained across another child failure");
            recovery_overrun(&h, 1, false);
        } else if (mode == 13) {
            JSClassID id = os64_js_class_id(&recovery_exotic_id);
            JSClassDef def = {.class_name="RecoveryExotic", .exotic=&recovery_exotic_methods};
            check(JS_NewClass(JS_GetRuntime(h.ctx[1]), id, &def) == 0, "native exotic fixture class created");
            JSValue global = JS_GetGlobalObject(h.ctx[1]);
            JS_SetPropertyStr(h.ctx[1], global, "exotic", JS_NewObjectClass(h.ctx[1], id));
            JS_FreeValue(h.ctx[1], global);
            JSValue fn = record_function(h.ctx[1], "(function(){return exotic.x})");
            check(os64_js_realm_task_begin(h.realm[1], OS64_JS_ABI_ID, "exotic", &out) == OS64_JS_OK &&
                  os64_js_realm_call(h.realm[0], OS64_JS_ABI_ID, fn, JS_UNDEFINED, 0, NULL, NULL, &out) == OS64_JS_LIMIT,
                  "native exotic deadline observed while child author frame remains live");
            JS_FreeValue(h.ctx[1], fn); issue = OS64_JS_FAILURE_NATIVE_SCOPE;
        } else {
            JSValue fn = realm_property(h.ctx[1], "bad"), value = JS_UNDEFINED;
            check(os64_js_realm_task_begin(h.realm[1], OS64_JS_ABI_ID, "raw-settings", &out) == OS64_JS_OK,
                  "raw settings scope overrun task opens");
            clock_step = 1000;
            check(JS_RunWithSettings(h.ctx[0], h.ctx[2], recovery_raw_settings_call, &fn, &value) &&
                  JS_IsException(value), "raw native settings body reenters and interrupts child bytecode");
            clock_step = 0;
            JS_FreeValue(h.ctx[0], value); JS_FreeValue(h.ctx[0], JS_GetException(h.ctx[0]));
            JS_FreeValue(h.ctx[1], fn); issue = OS64_JS_FAILURE_SETTINGS_SCOPE;
        }
        os64_js_recovery_info_t info;
        check(os64_js_recovery_inspect(h.runtime, OS64_JS_ABI_ID, &info) == OS64_JS_OK &&
              info.failure.original.status == OS64_JS_LIMIT &&
              info.failure.original.limit == (mode == 11 ? OS64_JS_LIMIT_JOBS : OS64_JS_LIMIT_EXECUTION),
              "scope matrix retains original execution or shared job-cap overrun");
        if (issue) check((info.failure.scope_issues & issue) && !info.failure.scopes_classified,
                         "unsupported scope records its exact exclusion instead of certifying completeness");
        if (mode != 5)
            check(os64_js_task_end(h.runtime, OS64_JS_ABI_ID, &out) == OS64_JS_LIMIT,
                  "scope matrix closes original abandoned task");
        os64_js_recovery_token_t token = {NULL, 81};
        os64_js_status_t result = os64_js_recovery_prepare(h.runtime, OS64_JS_ABI_ID, &token, &out);
        if (mode == 0) {
            check(result == OS64_JS_OK && info.failure.realm_count == 2 && h.cleaned == 6 &&
                  os64_js_recovery_commit(h.runtime, OS64_JS_ABI_ID, &token, &out) == OS64_JS_OK &&
                  os64_js_realm_is_failed(h.realm[1]) && os64_js_realm_is_failed(h.realm[2]) &&
                  realm_execute(h.realm[3], "box.value=46", &out) == OS64_JS_OK,
                  "complete cross-child stack seals both children and preserves independent sibling execution");
        } else {
            check(result != OS64_JS_OK && token.runtime == NULL && token.generation == 81 && !h.calls &&
                  os64_js_recovery_inspect(h.runtime, OS64_JS_ABI_ID, &info) == OS64_JS_OK &&
                  info.state == OS64_JS_RECOVERY_GLOBAL &&
                  os64_js_realm_task_begin(h.realm[3], OS64_JS_ABI_ID, "no-retry", &out) == OS64_JS_FAILED_RUNTIME,
                  "primary author, unsupported scope or pending rejection falls back before native containment");
            if (mode == 1) check(record_has(&info.failure, h.realm[0]) && info.failure.realm_count == 3,
                                 "primary entry alone is neutral but a primary author frame forces root cleanup");
        }
        if (pending_module) os64_js_realm_module_release(h.realm[3], OS64_JS_ABI_ID, pending_module, &out);
        if (foreign) JS_FreeContext(foreign);
        recovery_destroy(&h, before);
    }
}

static void recovery_admission_cases(void)
{
    size_t before = live;
    os64_js_config_t config = fixture_config();
    os64_js_outcome_t out;
    os64_js_runtime_t *runtime = (void *)1;
    os64_js_recovery_host_t host = {.prepare=recovery_host_prepare};
    check(os64_js_create_with_child_recovery(&config, OS64_JS_TEARDOWN_FATAL, OS64_JS_ABI_ID,
          NULL, &runtime, &out) == OS64_JS_BAD_ARGUMENT && !runtime,
          "recovery construction requires an explicit native consistency policy");
    check(os64_js_create_with_child_recovery(&config, OS64_JS_TEARDOWN_FATAL, "wrong", &host,
          &runtime, &out) == OS64_JS_ABI_MISMATCH && !runtime, "recovery construction validates ABI before publication");
    for (unsigned diagnostic = 0; diagnostic < 2; diagnostic++) {
        check((diagnostic ? os64_js_create_with_failure_records(&config, OS64_JS_TEARDOWN_FATAL,
              OS64_JS_ABI_ID, &runtime, &out) : os64_js_create(&config, OS64_JS_ABI_ID, &runtime, &out)) == OS64_JS_OK,
              "terminal constructor remains independently available");
        os64_js_recovery_info_t info, saved;
        memset(&info, 0xa5, sizeof(info)); saved = info;
        os64_js_recovery_token_t token = {NULL, 92};
        check(os64_js_recovery_inspect(runtime, OS64_JS_ABI_ID, &info) == OS64_JS_ADMISSION_REFUSED &&
              !memcmp(&info, &saved, sizeof(info)) &&
              os64_js_recovery_prepare(runtime, OS64_JS_ABI_ID, &token, &out) == OS64_JS_ADMISSION_REFUSED &&
              token.generation == 92 && os64_js_recovery_commit(runtime, OS64_JS_ABI_ID, &token, &out) == OS64_JS_ADMISSION_REFUSED,
              "default and record-only runtimes cannot opt in through recovery queries or tokens");
        os64_js_destroy(runtime);
    }
    check(live == before, "recovery admission refusals retain no allocations");
}
static void child_recovery_cases(void)
{
    size_t before = live;
    recovery_admission_cases();
    for (unsigned policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; policy++) {
        for (unsigned mode = 0; mode < 2; mode++) {
            RecoveryHarness h;
            if (!recovery_fixture(&h, policy)) continue;
            h.mode = mode == 0 ? 9 : 0;
            os64_js_outcome_t original = recovery_overrun(&h, 1, mode != 0), out;
            os64_js_recovery_info_t info;
            check(os64_js_recovery_inspect(h.runtime, OS64_JS_ABI_ID, &info) == OS64_JS_OK &&
                  info.state == OS64_JS_RECOVERY_ABORTED && info.generation && info.failure.realm_count == 1 &&
                  info.failure.realms[0] == h.realm[1] && info.failure.scopes_classified,
                  "complete child attribution creates an explicit recovery candidate");
            os64_js_recovery_token_t token = {NULL,97};
            if (!mode) {
                check(os64_js_recovery_prepare(h.runtime, OS64_JS_ABI_ID, &token, &out) == OS64_JS_BUSY &&
                      token.generation == 97 && !h.calls, "open abandoned task cannot acquire recovery preparation");
                check(os64_js_task_end(h.runtime, OS64_JS_ABI_ID, &out) == OS64_JS_LIMIT &&
                      out.limit == original.limit && out.jobs_executed == original.jobs_executed,
                      "original failed task closes without fresh accounting");
            }
            os64_js_recovery_token_t fake = {h.runtime,info.generation};
            check(os64_js_recovery_commit(h.runtime, OS64_JS_ABI_ID, &fake, &out) == OS64_JS_ADMISSION_REFUSED,
                  "generation alone cannot skip native preparation");
            size_t attempts_before = attempts;
            check(os64_js_recovery_prepare(h.runtime, OS64_JS_ABI_ID, &token, &out) == OS64_JS_OK && h.calls == 1 &&
                  token.runtime == h.runtime && token.generation == info.generation && h.cleaned == 2,
                  "native preparation seals the attributed child and returns exact-generation token");
            check(attempts == attempts_before, "engine and native preparation require no protocol allocations");
            check(os64_js_realm_task_begin(h.realm[2], OS64_JS_ABI_ID, "too-early", &out) == OS64_JS_FAILED_RUNTIME,
                  "prepared recovery does not admit sibling script before commit");
            check(os64_js_recovery_commit(h.runtime, OS64_JS_ABI_ID, &token, &out) == OS64_JS_OK && attempts == attempts_before,
                  "matching prepared generation commits without allocating or renewing a budget");
            check(out.jobs_executed == original.jobs_executed && original.jobs_executed == (mode == 0 ? 1u : 0u),
                  "commit preserves the abandoned task final nonzero executed-job charge");
            check(os64_js_recovery_commit(h.runtime, OS64_JS_ABI_ID, &token, &out) == OS64_JS_ADMISSION_REFUSED,
                  "recovery commit cannot be replayed");
            os64_js_failure_record_t after;
            record_observe(h.runtime, &after); record_original_same(&info.failure, &after);
            check(original.status == OS64_JS_LIMIT && original.limit == OS64_JS_LIMIT_EXECUTION &&
                  h.queue_count == 2 && h.queue[0] == 2 && h.queue[1] == 0,
                  "native sibling task order and original timeout outcome survive containment");
            for (unsigned i = 0; i < h.queue_count; i++) {
                unsigned who = h.queue[i];
                check(os64_js_realm_task_begin(h.realm[who], OS64_JS_ABI_ID, "independent-native-event", &out) == OS64_JS_OK,
                      "only a later independent native event opens a fresh sibling task");
                JSValue value = JS_UNDEFINED;
                check(os64_js_callback_with_settings(h.realm[who], OS64_JS_ABI_ID, &h.callback[who],
                      recovery_callback_call, NULL, &value, &out) == OS64_JS_OK,
                      "healthy captured callback remains callable after child containment");
                JS_FreeValue(h.ctx[who], value);
                check(os64_js_task_end(h.runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK,
                      "independent callback task closes under normal limits");
            }
            check(realm_execute(h.realm[2], "let refused=false;try{bad(true)}catch(e){refused=true}"
                  "if(!refused||box.value!==42||box.order!==31||box.forbidden!==undefined)throw 1;box.value=43", &out) == OS64_JS_OK,
                  "healthy sibling catches failed alias and retains shared data/mutation order");
            check(os64_js_realm_context(h.realm[1], OS64_JS_ABI_ID, &out) == NULL && out.status == OS64_JS_REALM_FAILED &&
                  realm_execute(h.realm[1], "throw 1", &out) == OS64_JS_REALM_FAILED,
                  "contained realm receives distinct non-sticky wrapper refusal");
            check(os64_js_realm_retire(h.realm[3], OS64_JS_ABI_ID, &out) == OS64_JS_OK,
                  "ordinary departure remains available for a separate child");
            JSValue retained = record_function(h.ctx[3], "(function(){return 7})");
            JSValue result = JS_Call(h.ctx[0], retained, JS_UNDEFINED, 0, NULL);
            check(!JS_IsException(result) && JS_VALUE_GET_INT(result) == 7,
                  "ordinary retired function remains directly callable unlike failed code");
            JS_FreeValue(h.ctx[0], result); JS_FreeValue(h.ctx[3], retained);
            if (mode == 0) {
                for (unsigned i = 4; i < OS64_JS_MAX_REALMS; i++) {
                    os64_js_realm_t *extra = NULL;
                    check(os64_js_realm_create(h.runtime, OS64_JS_ABI_ID, &extra, &out) == OS64_JS_OK && extra,
                          "new children consume fresh slots after failure without reusing failed backing");
                }
                os64_js_realm_t *extra = (void *)1;
                size_t prior = attempts;
                check(os64_js_realm_create(h.runtime, OS64_JS_ABI_ID, &extra, &out) == OS64_JS_REALM_LIMIT &&
                      !extra && attempts == prior && os64_js_realm_is_failed(h.realm[1]),
                      "failed and departed slots stay charged against permanent realm admission limit");
            }
            recovery_destroy(&h, before);
        }
        recovery_scope_cases(policy, before);
        recovery_health_cases(policy, before);
        recovery_busy_and_generations(policy, before);
    }
}
