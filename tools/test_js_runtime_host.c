/* Reuse R1's heap, syscall and fatal-exit fixtures. Its renamed entry keeps
 * those fixture checks available without running them in this wrapper suite. */
#define main js_port_fixture_main
#define os64_write js_port_fixture_write
#include "test_js_port_host.c"
#undef os64_write
#undef main
#include <pthread.h>
#include <sched.h>
#include "../userland/libjs/tests/cases.h"
#include "os64/io.h"

static const char *input_bytes;
static size_t input_length, input_position, read_limit = SIZE_MAX;
static unsigned input_opens, input_closes;
static int64_t open_error, read_error, close_error, output_error;
static size_t read_fail_at = SIZE_MAX, output_fail_at = SIZE_MAX;
static int64_t loading_time;
static int64_t monotonic = 10000000, clock_step;
static os64_js_runtime_t *cancel_on_read;

int64_t os64_open(const char *path, const char *mode)
{
    check(path != NULL && strcmp(mode, "r") == 0, "file helper opens read-only");
    input_opens++;
    input_position = 0;
    return open_error ? open_error : 73;
}
int64_t os64_read(int32_t handle, void *buffer, size_t size)
{
    check(handle == 73 && size != 0, "file helper reads its owned input");
    if (input_position >= read_fail_at) return read_error;
    size_t n = input_length - input_position;
    if (n > size) n = size;
    if (n > read_limit) n = read_limit;
    memcpy(buffer, input_bytes + input_position, n);
    input_position += n;
    monotonic += loading_time;
    if (cancel_on_read) os64_js_cancel(cancel_on_read);
    return (int64_t)n;
}
int64_t os64_close(int32_t handle)
{
    check(handle == 73, "file helper closes its owned input");
    input_closes++;
    return close_error;
}
int64_t os64_write(int32_t handle, const void *bytes, size_t size)
{
    if (handle != 2 && output_len >= output_fail_at) return output_error;
    return js_port_fixture_write(handle, bytes, size);
}

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

static void reset_io(const char *source)
{
    input_bytes = source;
    input_length = strlen(source);
    input_opens = input_closes = 0;
    open_error = read_error = close_error = output_error = 0;
    read_fail_at = output_fail_at = read_limit = SIZE_MAX;
    loading_time = 0;
    cancel_on_read = NULL;
    output_len = 0; write_limit = SIZE_MAX;
}

static void file_cases(void)
{
    os64_js_config_t config = fixture_config();
    os64_js_outcome_t outcome;
    reset_io("globalThis.loaded=0;Promise.resolve().then(()=>loaded=42)");
    read_limit = 7; loading_time = 2000000;
    os64_js_runtime_t *runtime = create_fixture(&config);
    if (!runtime) return;
    check(os64_js_run_file(runtime, "fixture.js", &outcome) == OS64_JS_OK &&
          outcome.jobs_executed == 1 && input_closes == 1 &&
          strcmp(outcome.source_name, "fixture.js") == 0,
          "short file reads drain jobs and loading time precedes deadline");
    loading_time = 0;
    check(execute_fixture(runtime, "if(loaded!==42)throw 1", &outcome) == OS64_JS_OK,
          "file source executed in owned context");
    check(os64_js_run_file(runtime, NULL, &outcome) == OS64_JS_BAD_ARGUMENT && input_opens == 1,
          "invalid file path has no IO side effects");
    const char *queued = "Promise.resolve().then(()=>{})";
    check(os64_js_eval(runtime, queued, strlen(queued), "queued", &outcome) == OS64_JS_OK && outcome.jobs_pending,
          "file busy case queues a turn");
    check(os64_js_run_file(runtime, "busy.js", &outcome) == OS64_JS_BUSY && input_opens == 1 && outcome.jobs_pending,
          "file busy refusal preserves queue without opening input");
    os64_js_destroy(runtime);
    for (unsigned mode = 0; mode < 5; mode++) {
        reset_io("globalThis.mustNotRun=true");
        if (mode == 0) open_error = -31;
        if (mode == 1) { read_error = -32; read_fail_at = 0; }
        if (mode == 2) close_error = OS64_CLOSE_NOT_COMMITTED;
        if (mode == 3) close_error = OS64_CLOSE_DEFERRED;
        if (mode == 4) { read_error = -32; read_fail_at = 0; close_error = OS64_CLOSE_DEFERRED; }
        runtime = create_fixture(&config);
        if (!runtime) return;
        int64_t expected = mode == 0 ? -31 : mode == 1 || mode == 4 ? -32 : close_error;
        check(os64_js_run_file(runtime, "errors.js", &outcome) == OS64_JS_HOST_FAILURE &&
              outcome.host_error == expected && input_closes == (mode != 0),
              "file failure preserves first service code and closes owned input");
        check(os64_js_eval(runtime, "true", 4, "after", &outcome) == OS64_JS_FAILED_RUNTIME,
              "file host failure retires runtime");
        os64_js_destroy(runtime);
    }
    config.limits.source_bytes = 4;
    for (unsigned mode = 0; mode < 3; mode++) {
        reset_io(mode == 0 ? "" : mode == 1 ? "true" : "true;");
        runtime = create_fixture(&config);
        if (!runtime) return;
        os64_js_status_t result = os64_js_run_file(runtime, "boundary.js", &outcome);
        check((mode < 2 ? result == OS64_JS_OK : result == OS64_JS_LIMIT && outcome.limit == OS64_JS_LIMIT_SOURCE) &&
              input_closes == 1, "empty, exact and oversized file boundaries close input");
        os64_js_destroy(runtime);
    }
    config = fixture_config();
    char source[12000]; memset(source, ' ', sizeof(source)); memcpy(source, "true", 4); source[sizeof(source)-1] = 0;
    for (unsigned mode = 0; mode < 2; mode++) {
        reset_io(source);
        runtime = create_fixture(&config);
        if (!runtime) return;
        fail_at = attempts + mode + 1;
        check(os64_js_run_file(runtime, "allocation.js", &outcome) == OS64_JS_HOST_FAILURE &&
              input_closes == 1, "file initial and growth allocation failures close input");
        fail_at = 0; os64_js_destroy(runtime);
        check(live == 0, "file allocation failure reclaims buffers and engine");
    }
    reset_io("throw Error('file exception')");
    runtime = create_fixture(&config);
    if (!runtime) return;
    check(os64_js_run_file(runtime, "throw.js", &outcome) == OS64_JS_EXCEPTION && input_closes == 1 &&
          strstr(outcome.message, "file exception"), "file exception retains primary diagnostic");
    check(execute_fixture(runtime, "true", &outcome) == OS64_JS_OK, "file script exception remains reusable");
    os64_js_destroy(runtime);
    reset_io("true");
    int64_t saved_time = monotonic;
    monotonic = INT64_MAX - 2500;
    config.limits.execution_ms = 1;
    runtime = create_fixture(&config);
    if (!runtime) { monotonic = saved_time; return; }
    check(execute_fixture(runtime, "Promise.resolve().then(()=>{})", &outcome) == OS64_JS_OK &&
          outcome.jobs_executed == 1, "file deadline overflow case has prior completed turn");
    loading_time = 1000;
    check(os64_js_run_file(runtime, "overflow.js", &outcome) == OS64_JS_BAD_ARGUMENT && input_closes == 1,
          "late file deadline overflow closes input and rejects execution");
    loading_time = 0;
    check(os64_js_drain_jobs(runtime, 1, &outcome) == OS64_JS_OK && outcome.jobs_executed == 1 &&
          strcmp(outcome.source_name, "runtime-fixture.js") == 0, "file bad argument preserves prior runtime state");
    os64_js_destroy(runtime); monotonic = saved_time;
    config = fixture_config(); reset_io("true");
    runtime = create_fixture(&config);
    if (!runtime) return;
    cancel_on_read = runtime;
    check(os64_js_run_file(runtime, "cancel.js", &outcome) == OS64_JS_CANCELLED && input_closes == 1,
          "cancellation between file reads closes input before execution");
    cancel_on_read = NULL; os64_js_destroy(runtime);
    char *large = malloc(700000);
    check(large != NULL, "large file fixture allocated");
    if (large) {
        memset(large, ' ', 699999); large[699999] = 0;
        reset_io(large); config.limits.memory_bytes = 1024*1024; config.limits.source_bytes = 1024*1024;
        runtime = create_fixture(&config);
        if (runtime) {
            check(os64_js_run_file(runtime, "memory.js", &outcome) == OS64_JS_LIMIT &&
                  outcome.limit == OS64_JS_LIMIT_MEMORY && input_closes == 1,
                  "file buffer growth shares runtime memory ceiling");
            os64_js_destroy(runtime);
        }
        free(large);
    }
    reset_io("");
}

static void output_cases(void)
{
    os64_js_config_t config = fixture_config();
    os64_js_outcome_t outcome;
    reset_io(""); write_limit = 2;
    helper_cases(42);
    const char expected[] = "hi 7 a\0b\n\nconverted\n";
    check(output_len == sizeof(expected)-1 && memcmp(output, expected, sizeof(expected)-1) == 0,
          "partial output writes preserve spacing, newline and embedded NUL");
    check(input_closes == 0, "output installer does not close borrowed handle");
    for (unsigned mode = 0; mode < 2; mode++) {
        reset_io("");
        os64_js_runtime_t *runtime = create_fixture(&config);
        if (!runtime) return;
        check(os64_js_install_output(runtime, 42, OS64_JS_OUTPUT_PRINT, &outcome) == OS64_JS_OK,
              "output failure capability setup");
        if (mode == 0) { output_fail_at = 0; output_error = -23; }
        else write_limit = 0;
        check(execute_fixture(runtime, "try{print('failed')}catch(e){}", &outcome) == OS64_JS_HOST_FAILURE &&
              outcome.host_error == (mode == 0 ? -23 : 0), "caught write failure and zero progress retire runtime");
        os64_js_destroy(runtime);
    }
    reset_io("");
    os64_js_runtime_t *runtime = create_fixture(&config);
    if (!runtime) return;
    check(os64_js_install_output(runtime, 42, OS64_JS_OUTPUT_PRINT, &outcome) == OS64_JS_OK,
          "conversion failure capability setup");
    check(execute_fixture(runtime, "print({toString(){throw Error('conversion')}})", &outcome) == OS64_JS_EXCEPTION &&
          strstr(outcome.message, "conversion") && output_len == 0, "output conversion exception remains a script result");
    check(execute_fixture(runtime, "print('after')", &outcome) == OS64_JS_OK && output_len == 6,
          "output usable after conversion exception");
    clock_step = 1000;
    check(execute_fixture(runtime, "print({toString(){for(;;){}}})", &outcome) == OS64_JS_LIMIT &&
          outcome.limit == OS64_JS_LIMIT_EXECUTION, "output conversion obeys active deadline");
    clock_step = 0; os64_js_destroy(runtime);
    reset_io("");
}

static unsigned console_getters;
static JSValue console_getter(JSContext *context, JSValueConst self, int argc, JSValueConst *args)
{
    (void)self; (void)argc; (void)args;
    console_getters++;
    return JS_NewObject(context);
}

static void setup_allocation_cases(void)
{
    os64_js_config_t config = fixture_config();
    os64_js_outcome_t outcome;
    const char *args[] = {"filename", "caf\xc3\xa9", "copied"};
    for (unsigned mode = 0; mode < 2; mode++) {
        os64_js_runtime_t *runtime = create_fixture(&config);
        if (!runtime) return;
        size_t before = attempts;
        os64_js_status_t result = mode == 0 ? os64_js_install_args(runtime, 3, args, &outcome) :
            os64_js_install_output(runtime, 42, OS64_JS_OUTPUT_PRINT | OS64_JS_OUTPUT_CONSOLE_LOG, &outcome);
        size_t inventory = attempts - before;
        check(result == OS64_JS_OK, "installer allocation inventory succeeds");
        os64_js_destroy(runtime);
        for (size_t i = 1; i <= inventory; i++) {
            runtime = create_fixture(&config);
            if (!runtime) return;
            fail_at = attempts + i;
            result = mode == 0 ? os64_js_install_args(runtime, 3, args, &outcome) :
                os64_js_install_output(runtime, 42, OS64_JS_OUTPUT_PRINT | OS64_JS_OUTPUT_CONSOLE_LOG, &outcome);
            fail_at = 0;
            check(result == OS64_JS_HOST_FAILURE, "installer allocation refusal is sticky host failure");
            os64_js_destroy(runtime);
            check(live == 0, "partial installer failure releases owned values");
        }
    }
    os64_js_runtime_t *runtime = create_fixture(&config);
    if (!runtime) return;
    JSContext *context = os64_js_context(runtime, OS64_JS_ABI_ID, &outcome);
    JSValue global = JS_GetGlobalObject(context);
    check(JS_DefinePropertyValueStr(context, global, "scriptArgs", JS_NewInt32(context, 9), 0) >= 0,
          "nonconfigurable setup target created");
    JS_FreeValue(context, global);
    check(os64_js_install_args(runtime, 3, args, &outcome) == OS64_JS_EXCEPTION,
          "failed argument property installation is reported");
    os64_js_destroy(runtime);
    runtime = create_fixture(&config);
    if (!runtime) return;
    context = os64_js_context(runtime, OS64_JS_ABI_ID, &outcome);
    global = JS_GetGlobalObject(context);
    check(JS_DefinePropertyValueStr(context, global, "print", JS_NewInt32(context, 9), 0) >= 0,
          "nonconfigurable output target created");
    JS_FreeValue(context, global);
    check(os64_js_install_output(runtime, 42, OS64_JS_OUTPUT_PRINT, &outcome) == OS64_JS_EXCEPTION,
          "failed output property installation is reported");
    os64_js_destroy(runtime);
    runtime = create_fixture(&config);
    if (!runtime) return;
    context = os64_js_context(runtime, OS64_JS_ABI_ID, &outcome);
    global = JS_GetGlobalObject(context);
    JSAtom name = JS_NewAtom(context, "console");
    check(JS_DefinePropertyGetSet(context, global, name, JS_NewCFunction(context, console_getter, "get console", 0),
          JS_UNDEFINED, JS_PROP_CONFIGURABLE) >= 0, "host console accessor created");
    JS_FreeAtom(context, name); JS_FreeValue(context, global);
    check(os64_js_install_output(runtime, 42, OS64_JS_OUTPUT_CONSOLE_LOG, &outcome) == OS64_JS_EXCEPTION &&
          console_getters == 0, "console setup rejects accessor without invoking its getter");
    os64_js_destroy(runtime);
    config.limits.memory_bytes = 1024*1024;
    runtime = create_fixture(&config);
    if (!runtime) return;
    char *large = malloc(2*1024*1024);
    check(large != NULL, "large argument fixture allocated");
    if (large) {
        memset(large, 'a', 2*1024*1024-1); large[2*1024*1024-1] = 0;
        const char *argument[] = {large};
        check(os64_js_install_args(runtime, 1, argument, &outcome) == OS64_JS_LIMIT &&
              outcome.limit == OS64_JS_LIMIT_MEMORY, "argument copy shares runtime memory budget");
        free(large);
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
    file_cases(); output_cases(); setup_allocation_cases();
    check(live == 0, "runtime suite releases every fixture allocation");
    printf("libjs runtime: %u checks, %u failures, %zu live allocations, %u native calls\n",
           checks, failures, live, native_calls);
    return failures != 0;
}
