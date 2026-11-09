#ifndef OS64_JS_TEARDOWN_CASES_H
#define OS64_JS_TEARDOWN_CASES_H

/* Shared target/host cases: use public binding operations and discard leaked
 * handles before destruction. The only finalizer writes a fixture counter. */
static unsigned teardown_finalized;
static void teardown_finalizer(JSRuntime *engine, JSValue value)
{
    (void)engine;
    (void)value;
    teardown_finalized++;
}

static void teardown_cases(void)
{
    os64_js_config_t config = fixture_config();
    os64_js_outcome_t outcome;
    os64_js_runtime_t *runtime = (void *)1;
    check(os64_js_create_with_teardown(&config, (os64_js_teardown_policy_t)99,
          OS64_JS_ABI_ID, &runtime, &outcome) == OS64_JS_BAD_ARGUMENT && runtime == NULL,
          "unknown teardown policy publishes no runtime");
    os64_js_teardown_report_t report = {.leaked = true, .reclaimed_blocks = 99};
    os64_js_destroy_report(NULL, &report);
    check(!report.leaked && report.reclaimed_blocks == 0 && report.reclaimed_bytes == 0,
          "NULL reporting destruction clears the report");
    static JSClassID class_slot;
    JSClassID class_id = os64_js_class_id(&class_slot);
    for (unsigned mode = 0; mode < 8; mode++) {
        runtime = NULL;
        check(os64_js_create_with_teardown(&config, OS64_JS_TEARDOWN_RECLAIM,
              OS64_JS_ABI_ID, &runtime, &outcome) == OS64_JS_OK,
              "reporting runtime constructed");
        if (runtime == NULL) return;
        JSContext *context = os64_js_context(runtime, OS64_JS_ABI_ID, &outcome);
        if (mode == 0) {
            check(execute_fixture(runtime,
                "globalThis.x={};x.self=x;Promise.resolve().then(()=>42);"
                "new RegExp('[a-z]+','u').test('abc');'abc'.normalize();JSON.stringify([1,2]);",
                &outcome) == OS64_JS_OK, "tracked runtime supports cycles jobs regexp unicode and JSON");
        } else if (mode == 1 || mode == 2) {
            const JSClassDef definition = {.class_name = "TeardownFixture", .finalizer = teardown_finalizer};
            check(JS_NewClass(JS_GetRuntime(context), class_id, &definition) == 0,
                  "resource-free fixture class registered");
            JSValue value = JS_NewObjectClass(context, class_id);
            check(!JS_IsException(value), "fixture object allocated");
            if (mode == 1) JS_FreeValue(context, value);
            /* In mode 2 the value is deliberately lost, including its finalizer. */
        } else if (mode == 3) {
            const char *source = "new WeakRef({})";
            JSValue value = JS_Eval(context, source, os64_strlen(source), "weakref", JS_EVAL_TYPE_GLOBAL);
            check(!JS_IsException(value), "weak-reference leak constructed");
        } else if (mode == 4) {
            check(js_malloc(context, 123) != NULL, "raw buffer leak constructed");
        } else if (mode == 5) {
            JSValue value = JS_NewString(context, "retained string outside the object lists");
            check(!JS_IsException(value), "string leak constructed");
        } else if (mode == 6) {
            check(os64_js_eval(runtime, "Promise.resolve().then(()=>{})", os64_strlen("Promise.resolve().then(()=>{})"), "pending", &outcome) == OS64_JS_OK &&
                  outcome.jobs_pending, "tracked runtime retains a pending job at destroy");
            os64_js_cancel(runtime);
        } else {
            check(execute_fixture(runtime, "throw Error('teardown')", &outcome) == OS64_JS_EXCEPTION,
                  "tracked runtime reports an exception before clean destroy");
        }
        unsigned before = teardown_finalized;
        os64_js_destroy_report(runtime, &report);
        bool leaked = mode >= 2 && mode <= 5;
        check(report.leaked == leaked && (leaked ? report.reclaimed_blocks > 0 && report.reclaimed_bytes > 0 :
              report.reclaimed_blocks == 0 && report.reclaimed_bytes == 0),
              "report distinguishes clean destruction from reclaimed object weakref buffer and string leaks");
        check(mode != 2 || teardown_finalized == before,
              "reclamation does not invoke the leaked object's finalizer");
    }
#ifdef JS_TEARDOWN_GUEST
    uint64_t before_bytes = 0, before_blocks = 0;
    bool sampled = teardown_guest_heap(&before_bytes, &before_blocks);
    check(sampled, "guest heap counters sampled before repeated leaks");
#endif
    bool repeated = true;
    size_t expected_blocks = 0, expected_bytes = 0;
    for (unsigned i = 0; i < 10000; i++) {
        runtime = NULL;
        if (os64_js_create_with_teardown(&config, OS64_JS_TEARDOWN_RECLAIM,
            OS64_JS_ABI_ID, &runtime, &outcome) != OS64_JS_OK) { repeated = false; break; }
        JSContext *context = os64_js_context(runtime, OS64_JS_ABI_ID, &outcome);
        if (JS_IsException(JS_NewObject(context))) repeated = false;
        os64_js_destroy_report(runtime, &report);
        if (i == 0) { expected_blocks = report.reclaimed_blocks; expected_bytes = report.reclaimed_bytes; }
        if (!report.leaked || report.reclaimed_blocks == 0 || report.reclaimed_bytes == 0 ||
            report.reclaimed_blocks != expected_blocks || report.reclaimed_bytes != expected_bytes)
            repeated = false;
#ifdef JS_TEARDOWN_HOST
        if (live != 0) repeated = false;
#endif
        if (!repeated) break;
    }
    check(repeated, "ten thousand leaked runtimes reclaimed without cumulative allocations");
#ifdef JS_TEARDOWN_GUEST
    uint64_t after_bytes = 0, after_blocks = 0;
    check(sampled && teardown_guest_heap(&after_bytes, &after_blocks) &&
          before_bytes == after_bytes && before_blocks == after_blocks,
          "guest repeated leaks preserve live heap bytes and block count");
    check(os64_heap_verify() == 0, "guest heap remains intact after repeated reclamation");
#endif
}
#endif
