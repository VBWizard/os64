#include <os64/js_retention.h>

static os64_js_runtime_t *retention_runtime;
static JSClassID retention_finalizer_slot;
static unsigned retention_finalizers, retention_jobs;
static bool retention_throw;

static os64_js_status_t retention_setup(os64_js_realm_t *realm, JSContext *ctx, void *opaque)
{
    (void)realm; (void)ctx; (void)opaque;
    os64_js_retention_t view = os64_js_retention_snapshot(retention_runtime, OS64_JS_ABI_ID);
    check(view.status == OS64_JS_OK && view.setup_active && view.active && view.staged_slots == 1,
          "retention: a real installation guard is visible without entering private setup");
    return OS64_JS_OK;
}
static JSValue retention_settings(JSContext *ctx, void *opaque)
{
    (void)ctx; (void)opaque;
    os64_js_retention_t view = os64_js_retention_snapshot(retention_runtime, OS64_JS_ABI_ID);
    check(view.status == OS64_JS_OK && view.settings_active && view.unknown == OS64_JS_RETENTION_UNKNOWN_ALL,
          "retention: checked settings activity does not imply enumerated callback context holds");
    return JS_UNDEFINED;
}
static JSValue retention_checkpoint(JSContext *ctx, int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    os64_js_retention_t view = os64_js_retention_snapshot(retention_runtime, OS64_JS_ABI_ID);
    check(view.status == OS64_JS_OK && view.active && view.task_open && view.turn_open,
          "retention: task checkpoint remains an active loan despite unknown job-role counts");
    return JS_UNDEFINED;
}

static JSValue retention_native(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self; (void)argc; (void)argv;
    if (retention_throw) JS_ThrowTypeError(ctx, "retention exception sentinel");
    os64_js_outcome_t saved = *fixture_outcome;
    size_t allocated = attempts;
    int64_t now = monotonic;
    os64_js_retention_t view = os64_js_retention_snapshot(retention_runtime, OS64_JS_ABI_ID);
    check(view.status == OS64_JS_OK && view.active && view.turn_open &&
          view.unknown == OS64_JS_RETENTION_UNKNOWN_ALL,
          "retention: native callback observes activity without a reclamation certificate");
    check(attempts == allocated && monotonic == now && !memcmp(&saved, fixture_outcome, sizeof(saved)),
          "retention: native inspection allocates nothing, polls no clock and preserves active outcome");
    check(JS_HasException(ctx) == retention_throw, "retention: pending engine exception is untouched");
    return retention_throw ? JS_EXCEPTION : JS_UNDEFINED;
}
static void retention_finalize(JSRuntime *engine, JSValue value)
{
    (void)engine; (void)value;
    os64_js_retention_t view = os64_js_retention_snapshot(retention_runtime, OS64_JS_ABI_ID);
    check(view.status == OS64_JS_FAILED_RUNTIME && view.destroying &&
          view.unknown == OS64_JS_RETENTION_UNKNOWN_ALL,
          "retention: destruction refuses before inspecting partial engine state");
    retention_finalizers++;
}
static JSValue retention_job(JSContext *ctx, int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    retention_jobs++;
    return JS_UNDEFINED;
}
static uint32_t retention_rejected(void *opaque, JSContext *ctx, JSValue promise)
{ (void)opaque; (void)ctx; (void)promise; return 71; }

static void retention_cases(void)
{
    for (unsigned policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; ++policy) {
        os64_js_config_t config = fixture_config();
        os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config, policy, OS64_JS_ABI_ID, &retention_runtime, &out) == OS64_JS_OK,
              "retention: both root teardown policies construct");
        if (!retention_runtime) continue;
        os64_js_retention_t view = os64_js_retention_snapshot(retention_runtime, OS64_JS_ABI_ID);
        check(view.status == OS64_JS_OK && view.occupied_slots == 1 && view.context_roots == 1 &&
              view.promise_then_roots == 1 && !view.retired_slots && !view.failed_slots &&
              view.unknown == OS64_JS_RETENTION_UNKNOWN_ALL,
              "retention: initial wrapper roots are measured without claiming all incoming edges");
        check(os64_js_retention_snapshot(NULL, OS64_JS_ABI_ID).status == OS64_JS_BAD_ARGUMENT &&
              os64_js_retention_snapshot(retention_runtime, NULL).status == OS64_JS_BAD_ARGUMENT &&
              os64_js_retention_snapshot(retention_runtime, "wrong ABI").status == OS64_JS_ABI_MISMATCH,
              "retention: invalid arguments and foreign ABI refuse scalar data");
        os64_js_realm_t *child, *staged;
        check(os64_js_realm_create(retention_runtime, OS64_JS_ABI_ID, &child, &out) == OS64_JS_OK &&
              os64_js_realm_stage(retention_runtime, OS64_JS_ABI_ID, 1u << 20, &staged, &out) == OS64_JS_OK,
              "retention: sibling and staged slot admitted");
        check(os64_js_realm_retire(child, OS64_JS_ABI_ID, &out) == OS64_JS_OK,
              "retention: lifecycle classification configured");
        size_t allocated = attempts;
        int64_t now = monotonic;
        os64_js_outcome_t saved = out;
        view = os64_js_retention_snapshot(retention_runtime, OS64_JS_ABI_ID);
        check(view.occupied_slots == 3 && view.context_roots == 3 && view.promise_then_roots == 3 &&
              view.retired_slots == 1 && view.staged_slots == 1 && !view.failed_slots,
              "retention: retirement and staging retain occupied slots and context roots");
        check(attempts == allocated && monotonic == now && !memcmp(&out, &saved, sizeof(out)),
              "retention: idle snapshot leaves allocation, clock and diagnostics unchanged");
        check(os64_js_realm_setup(staged, OS64_JS_ABI_ID, retention_setup, NULL, &out) == OS64_JS_OK,
              "retention: inspection leaves candidate setup usable");
        os64_js_realm_t *primary = os64_js_primary_realm(retention_runtime, OS64_JS_ABI_ID, &out);
        JSValue settings_value = JS_UNDEFINED;
        check(os64_js_task_begin(retention_runtime, OS64_JS_ABI_ID, "retention task", &out) == OS64_JS_OK,
              "retention: real host task supplies the checked-settings turn");
        check(os64_js_realm_with_settings(primary, primary, OS64_JS_ABI_ID, retention_settings, NULL,
              &settings_value, &out) == OS64_JS_OK, "retention: checked settings scope completes");
        JSContext *ctx = os64_js_context(retention_runtime, OS64_JS_ABI_ID, &out);
        JS_FreeValue(ctx, settings_value);
        check(JS_EnqueueJob(ctx, retention_checkpoint, 0, NULL) == 0 &&
              os64_js_checkpoint(retention_runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK &&
              os64_js_task_end(retention_runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK,
              "retention: observation does not replace task/checkpoint closure");
        os64_js_module_t *a = module_compile(retention_runtime, "https://retention.test/a", "export const a=1", &out);
        os64_js_module_t *b = module_compile(retention_runtime, "https://retention.test/b", "export const b=2", &out);
        view = os64_js_retention_snapshot(retention_runtime, OS64_JS_ABI_ID);
        check(a && b && view.module_owner_slots == 1,
              "retention: several module handles name one retaining owner, not a guessed module count");
        os64_js_module_release(retention_runtime, a);
        check(os64_js_retention_snapshot(retention_runtime, OS64_JS_ABI_ID).module_owner_slots == 1,
              "retention: remaining module handle retains its owner");
        os64_js_module_release(retention_runtime, b);
        check(!os64_js_retention_snapshot(retention_runtime, OS64_JS_ABI_ID).module_owner_slots,
              "retention: wrapper module presence clears on final release");
        check(install_native(retention_runtime, "inspectRetention", retention_native) >= 0,
              "retention: native inspection callback installed");
        retention_throw = false;
        check(execute_fixture(retention_runtime, "inspectRetention()", &out) == OS64_JS_OK,
              "retention: active native query leaves ordinary execution usable");
        retention_throw = true;
        check(execute_fixture(retention_runtime, "inspectRetention()", &out) == OS64_JS_EXCEPTION &&
              strstr(out.message, "retention exception sentinel"),
              "retention: inspection does not consume the original engine exception");
        retention_throw = false;
        os64_js_script_host_t host = {.rejected=retention_rejected};
        check(os64_js_set_script_host(retention_runtime, OS64_JS_ABI_ID, &host, &out) == OS64_JS_OK &&
              execute_fixture(retention_runtime, "Promise.reject(42)", &out) == OS64_JS_OK,
              "retention: script host retains rejection reporting records");
        check(os64_js_retention_snapshot(retention_runtime, OS64_JS_ABI_ID).rejection_records == 1,
              "retention: reading does not consume the rejection ledger");
        JSValue promise, reason;
        uint32_t tag = 0;
        check(os64_js_take_rejection(retention_runtime, OS64_JS_ABI_ID, &promise, &reason, &tag) && tag == 71,
              "retention: original rejection remains available to its host");
        ctx = os64_js_context(retention_runtime, OS64_JS_ABI_ID, &out);
        JS_FreeValue(ctx, promise); JS_FreeValue(ctx, reason);
        check(!os64_js_retention_snapshot(retention_runtime, OS64_JS_ABI_ID).rejection_records,
              "retention: ledger reports removal without a stale retained count");
        JSClassID id = os64_js_class_id(&retention_finalizer_slot);
        JSClassDef def = {.class_name="RetentionFinalizer",.finalizer=retention_finalize};
        check(JS_NewClass(JS_GetRuntime(ctx), id, &def) == 0, "retention: destruction hook registered");
        JSValue global = JS_GetGlobalObject(ctx);
        check(JS_SetPropertyStr(ctx, global, "retentionFinalizer", JS_NewObjectClass(ctx, id)) >= 0,
              "retention: engine owns the finalizer probe until root destruction");
        JS_FreeValue(ctx, global);
        retention_jobs = retention_finalizers = 0;
        check(JS_EnqueueJob(ctx, retention_job, 0, NULL) == 0, "retention: independent queued engine work created");
        view = os64_js_retention_snapshot(retention_runtime, OS64_JS_ABI_ID);
        check(view.jobs_pending && !retention_jobs && view.unknown == OS64_JS_RETENTION_UNKNOWN_ALL &&
              os64_js_quiescent(retention_runtime, OS64_JS_ABI_ID, &out) == OS64_JS_BUSY,
              "retention: pending work remains pending and prevents quiescence");
        os64_js_cancel(retention_runtime);
        view = os64_js_retention_snapshot(retention_runtime, OS64_JS_ABI_ID);
        check(view.status == OS64_JS_OK && view.cancelled && view.latched_status == OS64_JS_OK && !retention_jobs,
              "retention: pending cancellation is copied without advancing the health latch or queue");
        check(execute_fixture(retention_runtime, "42", &out) == OS64_JS_BUSY && !retention_jobs,
              "retention: pending work keeps its existing admission precedence over cancellation");
        check(JS_DiscardPendingJob(JS_GetRuntime(ctx)) == 1 && !retention_jobs,
              "retention: trusted fixture discards the pending job without executing it");
        check(execute_fixture(retention_runtime, "42", &out) == OS64_JS_CANCELLED,
              "retention: ordinary admission still observes cancellation");
        saved = out;
        view = os64_js_retention_snapshot(retention_runtime, OS64_JS_ABI_ID);
        check(view.status == OS64_JS_OK && view.latched_status == OS64_JS_CANCELLED &&
              view.context_roots == 3 && !memcmp(&saved, &out, sizeof(out)),
              "retention: failed wrapper remains inspectable without clearing sticky diagnostics");
        os64_js_teardown_report_t report;
        check(JS_EnqueueJob(ctx, retention_job, 0, NULL) == 0,
              "retention: raw queued cleanup fixture survives until root teardown");
        os64_js_destroy_report(retention_runtime, &report);
        check(retention_finalizers == 1 && !retention_jobs && !report.leaked && !live,
              "retention: root teardown discards queued work and leaves no engine allocations");
        retention_runtime = NULL;
        check(os64_js_create_with_teardown(&config, policy, OS64_JS_ABI_ID, &retention_runtime, &out) == OS64_JS_OK,
              "retention: allocation failure fixture constructs");
        ctx = os64_js_context(retention_runtime, OS64_JS_ABI_ID, &out);
        fail_at = attempts+1;
        void *refused = js_malloc_rt(JS_GetRuntime(ctx), 64u << 10);
        fail_at = 0;
        view = os64_js_retention_snapshot(retention_runtime, OS64_JS_ABI_ID);
        check(!refused && view.status == OS64_JS_OK && view.allocator_failures && view.latched_status == OS64_JS_OK,
              "retention: raw allocator failure is visible without observing or clearing its health latch");
        js_free_rt(JS_GetRuntime(ctx), refused);
        os64_js_destroy_report(retention_runtime, &report);
        check(!report.leaked && !live, "retention: allocator failure inspection leaves clean root teardown");
        retention_runtime = NULL;
    }
}
