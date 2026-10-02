/* Reuse R1's heap, syscall and fatal-exit fixtures. Its renamed entry keeps
 * those fixture checks available without running them in this wrapper suite. */
#define main js_port_fixture_main
#include "test_js_port_host.c"
#undef main
#include <pthread.h>
#include <sched.h>
#include "../userland/libjs/tests/cases.h"

static int64_t monotonic = 10000000, clock_step;
static int clock_error;
int64_t os64_micros(void)
{
    if (clock_error) return clock_error;
    monotonic += clock_step;
    return monotonic;
}

static void clock_cases(void)
{
    os64_js_config_t config = fixture_config();
    os64_js_outcome_t outcome;
    config.limits.execution_ms = 10;
    os64_js_runtime_t *runtime = create_fixture(&config);
    if (!runtime) return;
    clock_step = 1000;
    check(execute_fixture(runtime, "try{for(;;){}}catch(e){}", &outcome) == OS64_JS_LIMIT &&
          outcome.limit == OS64_JS_LIMIT_EXECUTION, "caught interrupt remains execution limit");
    clock_step = 0; os64_js_destroy(runtime);
    runtime = create_fixture(&config);
    if (!runtime) return;
    const char *source = "Promise.resolve().then(()=>{});";
    check(os64_js_eval(runtime, source, strlen(source), "deadline.js", &outcome) == OS64_JS_OK && outcome.jobs_pending,
          "deadline test queues work");
    monotonic += 10000;
    check(os64_js_drain_jobs(runtime, 0, &outcome) == OS64_JS_BAD_ARGUMENT && outcome.jobs_pending,
          "bad slice leaves expired turn unmodified");
    check(os64_js_eval(runtime, "true", 4, "busy", &outcome) == OS64_JS_BUSY,
          "busy evaluation cannot replace expired turn");
    check(os64_js_drain_jobs(runtime, 1, &outcome) == OS64_JS_LIMIT &&
          outcome.limit == OS64_JS_LIMIT_EXECUTION && outcome.jobs_executed == 0,
          "resumed slice keeps original deadline");
    os64_js_destroy(runtime);
    runtime = create_fixture(&config);
    if (!runtime) return;
    clock_step = 1000;
    check(execute_fixture(runtime, "throw {toString(){for(;;){}}}", &outcome) == OS64_JS_LIMIT &&
          outcome.limit == OS64_JS_LIMIT_EXECUTION, "exception conversion retains execution budget");
    clock_step = 0; os64_js_destroy(runtime);
    runtime = create_fixture(&config);
    if (!runtime) return;
    clock_error = -17;
    check(execute_fixture(runtime, "true", &outcome) == OS64_JS_HOST_FAILURE && outcome.host_error == -17,
          "runtime clock failure preserves host code");
    clock_error = 0; os64_js_destroy(runtime);
    clock_error = -19; runtime = (void *)1;
    check(os64_js_create(&config, OS64_JS_ABI_ID, &runtime, &outcome) == OS64_JS_HOST_FAILURE &&
          outcome.host_error == -19 && !runtime, "constructor clock failure publishes no runtime");
    clock_error = 0;
}

static void diagnostic_cases(void)
{
    os64_js_config_t config = fixture_config();
    os64_js_runtime_t *runtime = create_fixture(&config);
    if (!runtime) return;
    os64_js_outcome_t outcome;
    check(execute_fixture(runtime, "throw {toString(){throw Error('secondary')},get stack(){throw Error('stack')}}",
          &outcome) == OS64_JS_EXCEPTION && strstr(outcome.message, "unavailable"),
          "secondary diagnostic exceptions keep fixed fallback");
    check(execute_fixture(runtime, "true", &outcome) == OS64_JS_OK, "secondary exceptions are cleared");
    char name[512]; memset(name, 'n', sizeof(name)); name[sizeof(name)-1] = 0;
    const char *source = "throw Error('x'.repeat(2048))";
    check(os64_js_run(runtime, source, strlen(source), name, &outcome) == OS64_JS_EXCEPTION &&
          outcome.diagnostic_truncated && strlen(outcome.message) == OS64_JS_MESSAGE_CAP-1 &&
          strlen(outcome.source_name) == OS64_JS_SOURCE_NAME_CAP-1 &&
          outcome.line == 0 && outcome.column == 0, "diagnostics truncate and mark unavailable structured location");
    check(execute_fixture(runtime,
          "globalThis.touched=false;const e=new Error('safe');Object.defineProperty(e,'message',{get(){touched=true;throw 1}});"
          "Object.defineProperty(e,'stack',{get(){touched=true;throw 1}});Promise.reject(e)", &outcome) == OS64_JS_UNHANDLED_REJECTION,
          "rejection checkpoint avoids Error accessors");
    check(execute_fixture(runtime, "if(touched)throw Error('getter ran')", &outcome) == OS64_JS_OK,
          "rejection accessors did not execute");
    os64_js_destroy(runtime);
}

static void allocation_cases(void)
{
    os64_js_config_t config = fixture_config();
    os64_js_outcome_t outcome;
    for (size_t budget = 4096; budget <= 1024*1024; budget *= 2) {
        config.limits.memory_bytes = budget;
        os64_js_runtime_t *limited = NULL;
        os64_js_status_t result = os64_js_create(&config, OS64_JS_ABI_ID, &limited, &outcome);
        check((result == OS64_JS_LIMIT && outcome.limit == OS64_JS_LIMIT_MEMORY && !limited) ||
              (result == OS64_JS_OK && limited), "constructor ceiling uses recoverable memory verdict");
        os64_js_destroy(limited);
        check(live == 0, "constructor ceiling reclaims partial engine");
    }
    config = fixture_config();
    size_t start = attempts;
    os64_js_runtime_t *runtime = create_fixture(&config);
    size_t allocations = attempts - start;
    if (!runtime) return;
    os64_js_destroy(runtime);
    for (size_t i = 1; i <= allocations; i++) {
        fail_at = attempts + i;
        runtime = NULL;
        os64_js_status_t value = os64_js_create(&config, OS64_JS_ABI_ID, &runtime, &outcome);
        fail_at = 0;
        check(value == OS64_JS_HOST_FAILURE && !runtime && outcome.host_error == 0,
              "constructor allocation failure is unpublished and classified");
        if (runtime) os64_js_destroy(runtime);
        check(live == 0, "partial constructor reclaims native and engine storage");
    }
    runtime = create_fixture(&config);
    if (!runtime) return;
    char source[8192]; memset(source, ' ', sizeof(source)); memcpy(source, "true", 4);
    fail_at = attempts + 1;
    check(os64_js_run(runtime, source, sizeof(source), "allocation.js", &outcome) == OS64_JS_HOST_FAILURE,
          "source buffer allocation failure is host failure");
    fail_at = 0;
    check(os64_js_eval(runtime, "true", 4, "after", &outcome) == OS64_JS_FAILED_RUNTIME,
          "allocation failure retires runtime");
    os64_js_destroy(runtime);
}

static JSClassID shared_slot, distinct_slots[8], shared_ids[8];
static void *class_worker(void *opaque)
{
    size_t index = (size_t)opaque;
    for (size_t i = 0; i < 1000; i++) {
        shared_ids[index] = os64_js_class_id(&shared_slot);
        (void)os64_js_class_id(&distinct_slots[index]);
    }
    return NULL;
}
static unsigned cancel_ready;
static JSValue native_ready(JSContext *context, JSValueConst self, int argc, JSValueConst *args)
{
    (void)context; (void)self; (void)argc; (void)args;
    __atomic_store_n(&cancel_ready, 1, __ATOMIC_RELEASE);
    return JS_UNDEFINED;
}
static void *cancel_worker(void *opaque)
{
    while (!__atomic_load_n(&cancel_ready, __ATOMIC_ACQUIRE)) sched_yield();
    os64_js_cancel(opaque);
    return NULL;
}
static void thread_cases(void)
{
    pthread_t threads[8];
    size_t count = 0;
    for (; count < 8; count++) if (pthread_create(&threads[count], NULL, class_worker, (void *)count)) break;
    check(count == 8, "class workers created");
    for (size_t i = 0; i < count; i++) pthread_join(threads[i], NULL);
    for (size_t i = 0; i < count; i++) {
        check(shared_ids[i] == shared_slot && shared_slot && distinct_slots[i] != shared_slot,
              "concurrent class slot gets one shared ID");
        for (size_t j = 0; j < i; j++) check(distinct_slots[i] != distinct_slots[j], "distinct concurrent class IDs");
    }
    os64_js_config_t config = fixture_config();
    os64_js_runtime_t *runtime = create_fixture(&config);
    if (!runtime) return;
    check(install_native(runtime, "ready", native_ready) >= 0, "cancellation rendezvous installed");
    pthread_t worker;
    int created = pthread_create(&worker, NULL, cancel_worker, runtime);
    check(!created, "cancellation worker created");
    if (!created) {
        os64_js_outcome_t outcome;
        check(execute_fixture(runtime, "ready();for(;;){}", &outcome) == OS64_JS_CANCELLED,
              "cross-thread cancellation interrupts active owner");
        pthread_join(worker, NULL);
    }
    os64_js_destroy(runtime);
}
int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "--raw-construction")) {
        fail_at = attempts + (argc == 3 ? strtoul(argv[2], NULL, 10) : 12);
        JSRuntime *engine = JS_NewRuntime();
        JSContext *context = JS_NewContext(engine);
        fail_at = 0;
        if (context) JS_FreeContext(context);
        JS_FreeRuntime(engine);
        check(context == NULL && live == 0, "raw engine reclaims context prototype allocation failure");
        return failures != 0;
    }
    if (argc == 2) {
        os64_js_config_t config = fixture_config();
        fixture_runtime = create_fixture(&config);
        if (!fixture_runtime) return 1;
        if (!strcmp(argv[1], "--leak")) {
            os64_js_outcome_t outcome;
            JSContext *context = os64_js_context(fixture_runtime, OS64_JS_ABI_ID, &outcome);
            (void)JS_NewObject(context);
            os64_js_destroy(fixture_runtime);
        } else if (!strcmp(argv[1], "--active-destroy")) {
            os64_js_outcome_t outcome;
            install_native(fixture_runtime, "destroy", native_destroy);
            execute_fixture(fixture_runtime, "destroy()", &outcome);
        }
        return 1;
    }
    runtime_cases(); clock_cases(); diagnostic_cases(); allocation_cases(); thread_cases();
    check(live == 0, "runtime suite releases every fixture allocation");
    printf("libjs runtime: %u checks, %u failures, %zu live allocations, %u native calls\n",
           checks, failures, live, native_calls);
    return failures != 0;
}
