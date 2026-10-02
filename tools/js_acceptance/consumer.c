/* Consumer acceptance uses the published API and upstream assertions; it does
 * not include R2's cases or inspect private runtime/allocator state. */
#include "os64/os64.h"
#include "os64/js_engine.h"
#include "quickjs.h"
#include "language.h"

static unsigned checks, failures, finalized, marked;
static os64_js_runtime_t *active;
static os64_js_outcome_t *outer;
static JSClassID resource_class;
static int resource_live;
static JSValue resource_child;
static int destroying;

#ifdef JS_ACCEPTANCE_HOST
extern void acceptance_refuse_allocations(void);
extern int acceptance_host_failures(void);
#endif

static void check(int passed, const char *name)
{
    checks++;
    if (!passed) {
        failures++;
        os64_printf("FAIL embedding: %s\n", name);
    }
}

static int contains(const char *text, const char *needle)
{
    for (; *text; text++) {
        size_t i = 0;
        while (needle[i] && text[i] == needle[i]) i++;
        if (!needle[i]) return 1;
    }
    return 0;
}

static os64_js_config_t config(void)
{
    return (os64_js_config_t){.limits = {
        .memory_bytes = 64 * 1024 * 1024, .stack_bytes = 256 * 1024,
        .source_bytes = 1024 * 1024, .execution_ms = 60000,
        .jobs_per_turn = 2048
    }};
}

static os64_js_runtime_t *create(os64_js_config_t settings)
{
    os64_js_runtime_t *runtime = NULL;
    os64_js_outcome_t result;
    check(os64_js_create(&settings, OS64_JS_ABI_ID, &runtime, &result) == OS64_JS_OK &&
          result.status == OS64_JS_OK && runtime != NULL, "create publishes a usable runtime");
    return runtime;
}

static os64_js_status_t run(os64_js_runtime_t *runtime, const char *source,
                            os64_js_outcome_t *result)
{
    active = runtime;
    outer = result;
    os64_js_status_t status = os64_js_run(runtime, source, os64_strlen(source),
                                         "acceptance.js", result);
    outer = NULL;
    check(status == result->status, "returned status matches caller-owned outcome");
    check(result->message[OS64_JS_MESSAGE_CAP-1] == 0 &&
          result->source_name[OS64_JS_SOURCE_NAME_CAP-1] == 0 &&
          result->stack_trace[OS64_JS_STACK_TRACE_CAP-1] == 0,
          "diagnostic arrays retain terminal NULs");
    return status;
}

static JSValue triple(JSContext *context, JSValueConst self, int argc, JSValueConst *args)
{
    (void)self;
    int32_t value;
    if (argc != 1) return JS_ThrowTypeError(context, "one value required");
    if (JS_ToInt32(context, &value, args[0]) < 0) return JS_EXCEPTION;
    return JS_NewFloat64(context, (double)value * 3);
}

static JSValue reenter(JSContext *context, JSValueConst self, int argc, JSValueConst *args)
{
    (void)self; (void)argc; (void)args;
    os64_js_outcome_t nested;
    unsigned char snapshot[sizeof(*outer)];
    os64_memcpy(snapshot, outer, sizeof(snapshot));
    check(os64_js_context(active, OS64_JS_ABI_ID, &nested) == context,
          "callback borrows its context with its own outcome");
    check(os64_js_eval(active, "throw 1", 7, "nested", &nested) == OS64_JS_BUSY,
          "evaluation re-entry is refused");
    check(os64_js_drain_jobs(active, 1, &nested) == OS64_JS_BUSY,
          "job-drain re-entry is refused");
    check(os64_js_install_args(active, 0, NULL, &nested) == OS64_JS_BUSY,
          "setup re-entry is refused");
    check(os64_memcmp(&snapshot, outer, sizeof(snapshot)) == 0,
          "nested refusal preserves the active caller's outcome");
    return JS_UNDEFINED;
}

static JSValue cancel(JSContext *context, JSValueConst self, int argc, JSValueConst *args)
{
    (void)context; (void)self; (void)argc; (void)args;
    os64_js_cancel(active);
    return JS_UNDEFINED;
}

static int install(os64_js_runtime_t *runtime, const char *name, JSCFunction *callback)
{
    os64_js_outcome_t result;
    JSContext *context = os64_js_context(runtime, OS64_JS_ABI_ID, &result);
    if (!context) return -1;
    JSValue global = JS_GetGlobalObject(context);
    if (JS_IsException(global)) { check(0, "binding global object available"); return -1; }
    JSValue function = JS_NewCFunction(context, callback, name, 1);
    if (JS_IsException(function)) {
        JS_FreeValue(context, global);
        check(0, "binding function allocated");
        return -1;
    }
    int status = JS_SetPropertyStr(context, global, name, function);
    JS_FreeValue(context, global);
    check(status >= 0, "host installs its selected capability");
    return status;
}

static void capability_cases(void)
{
    os64_js_outcome_t result;
    os64_js_runtime_t *runtime = create(config());
    if (!runtime) return;
    if (install(runtime, "triple", triple) < 0 || install(runtime, "reenter", reenter) < 0) {
        os64_js_destroy(runtime);
        return;
    }
    check(run(runtime,
          "if(triple(14)!==42)throw Error('binding result');"
          "for(const name of ['print','console','scriptArgs','std','os','fetch',"
          "'require','Worker','Atomics','SharedArrayBuffer','__loadScript'])"
          "if(typeof globalThis[name]!=='undefined')throw Error('unexpected '+name);reenter();",
          &result) == OS64_JS_OK, "registered and absent capabilities are distinguished");
    check(run(runtime, "triple()", &result) == OS64_JS_EXCEPTION &&
          contains(result.message, "one value required"),
          "throwing native callback returns control and preserves its diagnostic");
    check(run(runtime, "if(triple(7)!==21)throw 1", &result) == OS64_JS_OK,
          "ordinary callback exception leaves the runtime reusable");
    check(run(runtime, "Promise.resolve().then(()=>reenter())", &result) == OS64_JS_OK &&
          result.jobs_executed == 1, "entry guard also holds inside a native Promise callback");
    check(run(runtime, "Promise.reject('unhandled marker')", &result) == OS64_JS_UNHANDLED_REJECTION &&
          !result.jobs_pending && contains(result.message, "unhandled marker"),
          "unhandled rejection is reported at the empty-queue checkpoint");
    check(run(runtime, "true", &result) == OS64_JS_OK,
          "reported rejection does not recur on the next turn");
    check(run(runtime, "globalThis.done=0;Promise.resolve().then(()=>done=42);throw Error('primary')",
          &result) == OS64_JS_EXCEPTION && result.jobs_pending,
          "recoverable script exception retains queued work");
    check(os64_js_eval(runtime, "true", 4, "premature", &result) == OS64_JS_BUSY,
          "pending turn refuses a new evaluation");
    check(os64_js_drain_jobs(runtime, 1, &result) == OS64_JS_OK && !result.jobs_pending &&
          result.jobs_executed == 1, "host can finish the interrupted turn");
    check(run(runtime, "if(done!==42)throw 1", &result) == OS64_JS_OK,
          "context remains reusable after draining retained work");
    os64_js_destroy(runtime);
}

static void setup_cases(void)
{
    os64_js_runtime_t *runtime = create(config());
    if (!runtime) return;
    os64_js_outcome_t result;
    if (install(runtime, "print", triple) < 0) { os64_js_destroy(runtime); return; }
    char name[] = "original.js", argument[] = "copied argument";
    const char *arguments[] = {name, argument};
    check(os64_js_install_args(runtime, 2, arguments, &result) == OS64_JS_OK,
          "host supplies its copied argument capability");
    name[0] = 'X'; argument[0] = 'X';
    check(os64_js_install_output(runtime, 1, OS64_JS_OUTPUT_CONSOLE_LOG, &result) == OS64_JS_OK,
          "host selects console.log output");
    check(run(runtime, "if(print(14)!==42||scriptArgs.length!==2||scriptArgs[0]!=='original.js'||"
          "scriptArgs[1]!=='copied argument')throw Error('setup ownership');"
          "console.log('Acceptance output marker');", &result) == OS64_JS_OK,
          "argument copies survive caller edits and console-only setup preserves print");
    os64_js_destroy(runtime);
    /* The host hook rejects close calls; the guest's successful write verifies
     * that the borrowed standard-output handle remains open after destruction. */
    static const char marker[] = "Borrowed output remains open\n";
    check(os64_write(1, marker, sizeof(marker)-1) == sizeof(marker)-1,
          "destruction leaves the borrowed output handle with the host");
}

static void finalizer(JSRuntime *engine, JSValue value)
{
    if (JS_GetOpaque(value, resource_class) != &resource_live) return;
    finalized++;
    resource_live = 0;
    JS_FreeValueRT(engine, resource_child);
    resource_child = JS_UNDEFINED;
    if (destroying) {
        os64_js_outcome_t result;
        check(os64_js_context(active, OS64_JS_ABI_ID, &result) == NULL &&
              result.status == OS64_JS_FAILED_RUNTIME, "destruction closes context access during native finalization");
    }
}

static void mark(JSRuntime *engine, JSValueConst value, JS_MarkFunc *visitor)
{
    if (JS_GetOpaque(value, resource_class) != &resource_live) return;
    marked++;
    JS_MarkValue(engine, resource_child, visitor);
}

static int publish_resource(os64_js_runtime_t *runtime)
{
    static const JSClassDef definition = {
        .class_name = "AcceptanceResource", .finalizer = finalizer, .gc_mark = mark
    };
    os64_js_outcome_t result;
    JSContext *context = os64_js_context(runtime, OS64_JS_ABI_ID, &result);
    if (!context) return -1;
    JSRuntime *engine = JS_GetRuntime(context);
    JSClassID id = os64_js_class_id(&resource_class);
    if (JS_NewClass(engine, id, &definition) < 0) return -1;
    JSValue object = JS_NewObjectClass(context, id);
    resource_child = JS_NewObject(context);
    if (JS_IsException(object) || JS_IsException(resource_child)) {
        JS_FreeValue(context, object);
        JS_FreeValue(context, resource_child);
        resource_child = JS_UNDEFINED;
        return -1;
    }
    resource_live = 1;
    JS_SetOpaque(object, &resource_live);
    /* The native edge closes a cycle. Its marking hook accounts for the child
     * reference, and the finalizer releases it with the runtime-level API. */
    int status = JS_SetPropertyStr(context, resource_child, "parent", JS_DupValue(context, object));
    JSValue global = JS_GetGlobalObject(context);
    if (status >= 0) status = JS_SetPropertyStr(context, global, "resource", JS_DupValue(context, object));
    JS_FreeValue(context, global);
    JS_FreeValue(context, object);
    return status;
}

static void ownership_cases(void)
{
    unsigned before = finalized;
    os64_js_runtime_t *runtime = create(config());
    if (!runtime) return;
    active = runtime;
    int installed = publish_resource(runtime) >= 0;
    check(installed, "native class and cyclic resource installed");
    if (!installed) { os64_js_destroy(runtime); return; }
    os64_js_outcome_t result;
    JSContext *context = os64_js_context(runtime, OS64_JS_ABI_ID, &result);
    JS_RunGC(JS_GetRuntime(context));
    check(resource_live && finalized == before && marked != 0,
          "marked native child and rooted resource survive collection");
    check(run(runtime, "resource=null", &result) == OS64_JS_OK, "script releases its resource root");
    JS_RunGC(JS_GetRuntime(context));
    check(!resource_live && finalized == before + 1,
          "unreachable native cycle finalizes once and releases its child");
    os64_js_destroy(runtime);
    check(finalized == before + 1, "destroy does not repeat an already collected finalizer");

    runtime = create(config());
    if (!runtime) return;
    active = runtime;
    installed = publish_resource(runtime) >= 0;
    check(installed, "teardown resource installed");
    if (!installed || install(runtime, "cancel", cancel) < 0) { os64_js_destroy(runtime); return; }
    check(run(runtime, "Promise.resolve().then(()=>{throw Error('discarded')});cancel();for(;;){}",
          &result) == OS64_JS_CANCELLED, "cancellation interrupts execution with queued work");
    check(run(runtime, "throw Error('must not execute')", &result) == OS64_JS_FAILED_RUNTIME,
          "cancelled runtime refuses reuse");
    destroying = 1;
    os64_js_destroy(runtime);
    destroying = 0;
    check(!resource_live && finalized == before + 2,
          "cancelled runtime teardown finalizes native resources once");
}

static void failure_cases(void)
{
    os64_js_outcome_t result;
    os64_js_config_t settings = config();
    settings.limits.jobs_per_turn = 7;
    os64_js_runtime_t *runtime = create(settings);
    if (!runtime) return;
    check(run(runtime, "function spin(){Promise.resolve().then(spin)}spin()", &result) == OS64_JS_LIMIT &&
          result.limit == OS64_JS_LIMIT_JOBS && result.jobs_executed == 7,
          "runaway Promise chain stops at the total turn cap");
    check(run(runtime, "true", &result) == OS64_JS_FAILED_RUNTIME,
          "job limit retires runtime");
    os64_js_destroy(runtime);

    runtime = create(config());
    if (!runtime) return;
    /* A valid integer that is not an open handle lets the real guest syscall
     * supply the write failure; host fixtures supply their own negative code. */
    check(os64_js_install_output(runtime, INT32_MAX, OS64_JS_OUTPUT_PRINT, &result) == OS64_JS_OK,
          "output handle is borrowed without a write during installation");
    check(run(runtime, "try{print('write marker')}catch(e){}", &result) == OS64_JS_HOST_FAILURE &&
          result.host_error < 0, "caught output exception remains a host failure");
    check(run(runtime, "true", &result) == OS64_JS_FAILED_RUNTIME, "output failure retires runtime");
    os64_js_destroy(runtime);

    settings = config(); settings.limits.memory_bytes = 1024 * 1024;
    runtime = create(settings);
    if (!runtime) return;
    check(run(runtime, "throw {toString(){return 'x'.repeat(100000000)}}", &result) == OS64_JS_LIMIT &&
          result.limit == OS64_JS_LIMIT_MEMORY && result.message[0] != 0,
          "memory exhaustion during error conversion returns a bounded diagnostic");
    check(run(runtime, "true", &result) == OS64_JS_FAILED_RUNTIME,
          "diagnostic memory limit retires runtime");
    os64_js_destroy(runtime);
}

static void compatibility_cases(void)
{
    os64_js_config_t settings = config();
    os64_js_outcome_t result;
    os64_js_runtime_t *runtime = (void *)1;
    check(os64_js_create(&settings, "os64-js/incompatible", &runtime, &result) == OS64_JS_ABI_MISMATCH &&
          runtime == NULL, "header/library mismatch publishes no runtime");
    runtime = create(settings);
    if (!runtime) return;
    check(os64_js_context(runtime, "binding/incompatible", &result) == NULL &&
          result.status == OS64_JS_ABI_MISMATCH, "separately compiled binding mismatch publishes no context");
    check(run(runtime, "true", &result) == OS64_JS_OK, "ABI refusal preserves existing runtime");
    os64_js_destroy(runtime);
}

#ifdef JS_ACCEPTANCE_HOST
static JSValue refuse(JSContext *context, JSValueConst self, int argc, JSValueConst *args)
{
    (void)self;
    if (argc != 1) return JS_ThrowTypeError(context, "one reason required");
    acceptance_refuse_allocations();
    return JS_Throw(context, JS_DupValue(context, args[0]));
}

static void host_exhaustion_case(void)
{
    os64_js_runtime_t *runtime = create(config());
    if (!runtime) return;
    if (install(runtime, "refuse", refuse) < 0) { os64_js_destroy(runtime); return; }
    os64_js_outcome_t result;
    check(run(runtime, "refuse({toString(){return 'x'.repeat(512)}})", &result) == OS64_JS_HOST_FAILURE &&
          result.host_error == 0 && result.message[0] != 0,
          "OS allocation refusal during diagnostics keeps structured host failure");
    check(run(runtime, "true", &result) == OS64_JS_FAILED_RUNTIME,
          "OS allocation refusal retires runtime");
    os64_js_destroy(runtime);
}
#endif

static void language(void)
{
    unsigned passed = 0, failed = 0, skipped = 0;
    for (size_t i = 0; i < sizeof(language_cases)/sizeof(*language_cases); i++) {
        const struct language_case *test = &language_cases[i];
        if (test->skip) {
            skipped++;
            os64_printf("SKIP upstream %s: %s\n", test->name, test->skip);
            continue;
        }
        os64_js_runtime_t *runtime = create(config());
        os64_js_outcome_t result;
        int ok = runtime != NULL;
        if (ok) ok = run(runtime, test->definitions, &result) == OS64_JS_OK;
        if (ok) ok = run(runtime, test->call, &result) == OS64_JS_OK;
        if (ok) {
            passed++;
            os64_printf("PASS upstream %s\n", test->name);
        } else {
            failed++;
            os64_printf("FAIL upstream %s: %s\n", test->name,
                        runtime ? result.message : "runtime creation failed");
        }
        os64_js_destroy(runtime);
    }
    failures += failed;
    os64_printf("Upstream: %u passed, %u failed, %u skipped\n", passed, failed, skipped);
    os64_js_runtime_t *runtime = create(config());
    if (!runtime) return;
    os64_js_outcome_t result;
    check(run(runtime, language_cases[0].definitions, &result) == OS64_JS_OK &&
          run(runtime, "assert(1, 2)", &result) == OS64_JS_EXCEPTION &&
          contains(result.message, "assertion failed"),
          "negative control rejects a deliberately incorrect upstream expected value");
    os64_js_destroy(runtime);
}

int main(int argc, char **argv)
{
#ifdef JS_ACCEPTANCE_HOST
    if (argc == 2 && os64_streq(argv[1], "--diagnostic-oom")) {
        host_exhaustion_case();
        failures += (unsigned)acceptance_host_failures();
        return failures ? 1 : 0;
    }
#endif
    if (argc == 2 && os64_streq(argv[1], "--fatal-leak")) {
        os64_js_runtime_t *runtime = create(config());
        if (!runtime) return 1;
        os64_js_outcome_t result;
        JSContext *context = os64_js_context(runtime, OS64_JS_ABI_ID, &result);
        (void)JS_NewObject(context);
        os64_js_destroy(runtime);
        return 1; /* Fatal invariant handling must terminate this process. */
    }
    if (argc != 1) return 2;
    compatibility_cases(); capability_cases(); setup_cases(); ownership_cases(); failure_cases();
#ifdef JS_ACCEPTANCE_HOST
    host_exhaustion_case();
    failures += (unsigned)acceptance_host_failures();
#endif
    language();
    os64_printf("Acceptance: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
