#ifndef OS64_JS_RUNTIME_CASES_H
#define OS64_JS_RUNTIME_CASES_H
#include "os64/js_engine.h"
#include "os64/str.h"
#include "quickjs.h"

static const char *fixture_contains(const char *text, const char *needle)
{
    for (; *text; text++) {
        size_t i = 0;
        while (needle[i] && text[i] && needle[i] == text[i]) i++;
        if (!needle[i]) return text;
    }
    return NULL;
}

/* These cases run on one execution owner. Concurrent class-slot and
 * cancellation cases belong to the host driver, with separate native state. */
static os64_js_runtime_t *fixture_runtime;
static os64_js_outcome_t *fixture_outcome;
static unsigned native_calls;
static unsigned finalized;

static void fixture_finalizer(JSRuntime *engine, JSValue value)
{
    (void)engine;
    os64_js_runtime_t *runtime = JS_GetOpaque(value, JS_GetClassID(value));
    os64_js_outcome_t outcome;
    check(os64_js_eval(runtime, "true", 4, "finalizer", &outcome) == OS64_JS_BUSY,
          "teardown finalizer cannot start wrapper execution");
    check(os64_js_context(runtime, OS64_JS_ABI_ID, &outcome) == NULL &&
          outcome.status == OS64_JS_FAILED_RUNTIME, "teardown hides its closing context");
    finalized++;
}

static int install_class(os64_js_runtime_t *runtime)
{
    static JSClassID slot;
    os64_js_outcome_t outcome;
    JSContext *context = os64_js_context(runtime, OS64_JS_ABI_ID, &outcome);
    if (context == NULL) return -1;
    JSClassID id = os64_js_class_id(&slot);
    const JSClassDef definition = {.class_name = "RuntimeFixture", .finalizer = fixture_finalizer};
    if (JS_NewClass(JS_GetRuntime(context), id, &definition) < 0) return -1;
    JSValue value = JS_NewObjectClass(context, id);
    if (JS_IsException(value)) return -1;
    JS_SetOpaque(value, runtime);
    JSValue global = JS_GetGlobalObject(context);
    int result = JS_SetPropertyStr(context, global, "nativeObject", value);
    JS_FreeValue(context, global);
    return result;
}

static os64_js_config_t fixture_config(void)
{
    return (os64_js_config_t){.limits = {16 * 1024 * 1024, 256 * 1024,
                                        64 * 1024, 1000, 100}};
}

static JSValue native_twice(JSContext *context, JSValueConst self, int argc,
                            JSValueConst *argv)
{
    (void)self;
    native_calls++;
    if (argc != 1) return JS_ThrowTypeError(context, "twice needs one argument");
    int32_t value;
    if (JS_ToInt32(context, &value, argv[0]) < 0) return JS_EXCEPTION;
    return JS_NewFloat64(context, 2.0 * value);
}

static JSValue native_reentry(JSContext *context, JSValueConst self, int argc,
                              JSValueConst *argv)
{
    (void)self; (void)argc; (void)argv;
    os64_js_outcome_t nested;
    os64_js_status_t a = os64_js_eval(fixture_runtime, "true", 4, "nested", &nested);
    os64_js_status_t b = os64_js_drain_jobs(fixture_runtime, 1, &nested);
    os64_js_outcome_t access;
    JSContext *borrowed = os64_js_context(fixture_runtime, OS64_JS_ABI_ID, &access);
    os64_js_outcome_t before = *fixture_outcome;
    os64_js_status_t alias = os64_js_eval(fixture_runtime, "true", 4, "alias", fixture_outcome);
    JSContext *aliased_context = os64_js_context(fixture_runtime, OS64_JS_ABI_ID, fixture_outcome);
    return JS_NewBool(context, a == OS64_JS_BUSY && b == OS64_JS_BUSY &&
                               borrowed == context && access.status == OS64_JS_OK &&
                               alias == OS64_JS_BUSY && aliased_context == NULL &&
                               os64_memcmp(&before, fixture_outcome, sizeof(before)) == 0);
}

static JSValue native_cancel(JSContext *context, JSValueConst self, int argc,
                             JSValueConst *argv)
{
    (void)context; (void)self; (void)argc; (void)argv;
    os64_js_cancel(fixture_runtime);
    return JS_UNDEFINED;
}

static JSValue native_destroy(JSContext *context, JSValueConst self, int argc, JSValueConst *args)
{
    (void)context; (void)self; (void)argc; (void)args;
    os64_js_destroy(fixture_runtime); return JS_UNDEFINED;
}
static int install_native(os64_js_runtime_t *runtime, const char *name, JSCFunction *function)
{
    os64_js_outcome_t outcome;
    JSContext *context = os64_js_context(runtime, OS64_JS_ABI_ID, &outcome);
    if (context == NULL) return -1;
    JSValue global = JS_GetGlobalObject(context);
    JSValue value = JS_NewCFunction(context, function, name, 1);
    if (JS_IsException(value)) { JS_FreeValue(context, global); return -1; }
    int result = JS_SetPropertyStr(context, global, name, value);
    JS_FreeValue(context, global);
    return result;
}

static os64_js_runtime_t *create_fixture(const os64_js_config_t *config)
{
    os64_js_runtime_t *runtime = NULL;
    os64_js_outcome_t outcome;
    check(os64_js_create(config, OS64_JS_ABI_ID, &runtime, &outcome) == OS64_JS_OK &&
          runtime != NULL && outcome.status == OS64_JS_OK, "runtime creation");
    return runtime;
}

/* Measure retained target allocations rather than assuming the host and guest
 * heaps or the instrumented engine have the same construction footprint.
 * Reserve the input buffer, compiler source storage and small parsing work. */
static os64_js_config_t file_boundary_config(size_t capacity)
{
    os64_js_config_t config = fixture_config();
    os64_js_runtime_t *runtime = create_fixture(&config);
    if (runtime) {
        os64_js_outcome_t outcome;
        JSContext *context = os64_js_context(runtime, OS64_JS_ABI_ID, &outcome);
        JSMemoryUsage usage;
        JS_ComputeMemoryUsage(JS_GetRuntime(context), &usage);
        config.limits.memory_bytes = os64_malloc_size(runtime) + usage.malloc_size + 2 * capacity + 2048;
        os64_js_destroy(runtime);
    }
    return config;
}

static os64_js_status_t execute_fixture(os64_js_runtime_t *runtime, const char *source,
                                       os64_js_outcome_t *outcome)
{
    fixture_outcome = outcome;
    os64_js_status_t result = os64_js_run(runtime, source, os64_strlen(source), "runtime-fixture.js", outcome);
    fixture_outcome = NULL;
    check(result == outcome->status, "return status matches outcome");
    return result;
}

static JSValue native_budget(JSContext *context, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)context; (void)self; (void)argc; (void)argv;
    native_calls++;
    os64_js_outcome_t out;
    check(os64_js_check_budget(fixture_runtime, OS64_JS_ABI_ID, fixture_outcome) == OS64_JS_BUSY,
          "budget check refuses an alias of the outer outcome");
    check(os64_js_check_budget(fixture_runtime, "wrong-binding", &out) == OS64_JS_ABI_MISMATCH,
          "native budget boundary checks caller ABI");
    check(os64_js_check_budget(fixture_runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK &&
          out.status == OS64_JS_OK, "native budget check preserves a live outer turn");
    return JS_UNDEFINED;
}

static void lifecycle_cases(void)
{
    os64_js_config_t config = fixture_config();
    os64_js_outcome_t outcome;
    os64_js_runtime_t *runtime = (os64_js_runtime_t *)(uintptr_t)1;
    check(os64_js_create(&config, "wrong-header", &runtime, &outcome) == OS64_JS_ABI_MISMATCH &&
          runtime == NULL && outcome.status == OS64_JS_ABI_MISMATCH, "ABI mismatch publishes no runtime");
    config.limits.execution_ms = UINT64_MAX;
    check(os64_js_create(&config, OS64_JS_ABI_ID, &runtime, &outcome) == OS64_JS_BAD_ARGUMENT && !runtime,
          "unrepresentable deadline refused");
    config = fixture_config(); config.limits.memory_bytes = 1;
    check(os64_js_create(&config, OS64_JS_ABI_ID, &runtime, &outcome) == OS64_JS_LIMIT &&
          outcome.limit == OS64_JS_LIMIT_MEMORY && !runtime, "constructor memory ceiling");
    config = fixture_config(); config.limits.source_bytes = 0;
    check(os64_js_create(&config, OS64_JS_ABI_ID, &runtime, &outcome) == OS64_JS_BAD_ARGUMENT && !runtime,
          "zero configuration limit refused");
    config = fixture_config();
    for (unsigned i = 0; i < 12; i++) {
        runtime = create_fixture(&config);
        if (runtime == NULL) break;
        os64_js_outcome_t access;
        check(os64_js_context(runtime, "wrong-binding", &access) == NULL &&
              access.status == OS64_JS_ABI_MISMATCH, "per-binding ABI check");
        fixture_runtime = runtime;
        check(os64_js_check_budget(runtime, OS64_JS_ABI_ID, &access) == OS64_JS_BAD_ARGUMENT,
              "native budget check refuses idle runtime work");
        check(install_native(runtime, "checkBudget", native_budget) >= 0,
              "budget boundary fixture installed");
        check(execute_fixture(runtime, "checkBudget()", &outcome) == OS64_JS_OK,
              "callback-safe budget check does not finish the outer turn");
        check(install_native(runtime, "twice", native_twice) >= 0 &&
              install_native(runtime, "reenter", native_reentry) >= 0, "explicit native installation");
        unsigned before_finalization = finalized;
        check(install_class(runtime) >= 0, "native class registered in each runtime");
        check(execute_fixture(runtime,
              "if (twice(21)!==42 || !reenter()) throw Error('native capability');"
              "if (typeof print!=='undefined' || typeof console!=='undefined' ||"
              "typeof std!=='undefined' || typeof os!=='undefined' || typeof Worker!=='undefined' ||"
              "typeof Atomics!=='undefined' || typeof SharedArrayBuffer!=='undefined') throw Error('capability leak');"
              "if (typeof ArrayBuffer!=='function' || typeof Uint8Array!=='function' ||"
              "typeof Promise!=='function' || typeof Date!=='function') throw Error('missing language builtins');"
              "if (Math.sqrt(81)!==9 || Math.acosh(-49152)===Math.acosh(-49152) ||"
              "!Object.is(Math.round(-0.5),-0)) throw Error('maths integration');", &outcome) == OS64_JS_OK,
              "native result, re-entry guard, absent capabilities and real maths");
        check(execute_fixture(runtime, "twice()", &outcome) == OS64_JS_EXCEPTION &&
              fixture_contains(outcome.message, "twice needs one argument") != NULL &&
              fixture_contains(outcome.stack_trace, "runtime-fixture.js") != NULL,
              "callback exception and diagnostic source");
        check(execute_fixture(runtime, "if(twice(3)!==6) throw Error('reuse')", &outcome) == OS64_JS_OK,
              "runtime reusable after exception");
        check(os64_js_eval(runtime, NULL, 1, "bad", &outcome) == OS64_JS_BAD_ARGUMENT,
              "invalid source refused without execution");
        check(os64_js_eval(runtime, NULL, 0, "empty", &outcome) == OS64_JS_OK, "empty source");
        check(execute_fixture(runtime, "let broken = ;", &outcome) == OS64_JS_EXCEPTION &&
              outcome.message[0] != 0, "syntax exception diagnostics");
        check(execute_fixture(runtime, "try{(function recurse(){recurse()})()}catch(e){}", &outcome) == OS64_JS_OK,
              "stack overflow can be caught without text classification");
        os64_js_destroy(runtime);
        check(finalized == before_finalization + 1, "native class finalizer runs before engine release");
        fixture_runtime = NULL;
    }
    static JSClassID slot;
    JSClassID id = os64_js_class_id(&slot);
    check(id != 0 && os64_js_class_id(&slot) == id && os64_js_class_id(NULL) == 0,
          "process-lifetime class slot reuse");
    os64_js_destroy(NULL);
}

static void job_cases(void)
{
    os64_js_config_t config = fixture_config();
    os64_js_runtime_t *runtime = create_fixture(&config);
    if (runtime == NULL) return;
    os64_js_outcome_t outcome;
    const char *source = "globalThis.result=0;Promise.resolve(21).then(x=>result=twice(x));throw Error('keep jobs')";
    check(install_native(runtime, "twice", native_twice) >= 0, "job capability installation");
    check(os64_js_eval(runtime, source, os64_strlen(source), "queued.js", &outcome) == OS64_JS_EXCEPTION &&
          outcome.jobs_pending && outcome.jobs_executed == 0, "queued jobs survive evaluation exception");
    check(os64_js_eval(runtime, "true", 4, "new", &outcome) == OS64_JS_BUSY && outcome.jobs_pending,
          "new evaluation cannot replace queued turn");
    check(os64_js_drain_jobs(runtime, 0, &outcome) == OS64_JS_BAD_ARGUMENT && outcome.jobs_pending,
          "invalid slice preserves queued work");
    check(os64_js_drain_jobs(runtime, 1, &outcome) == OS64_JS_OK &&
          !outcome.jobs_pending && outcome.jobs_executed == 1, "manual drain after exception");
    check(execute_fixture(runtime, "if(result!==42)throw Error('wrong result')", &outcome) == OS64_JS_OK,
          "native capability completes queued job");
    check(execute_fixture(runtime, "Promise.reject(new Error('unhandled'))", &outcome) == OS64_JS_UNHANDLED_REJECTION &&
          fixture_contains(outcome.message, "unhandled") != NULL && !outcome.jobs_pending,
          "unhandled rejection at empty queue");
    check(execute_fixture(runtime, "true", &outcome) == OS64_JS_OK, "reported rejection is not repeated");
    source = "globalThis.p=Promise.reject('handled-later');Promise.resolve().then(()=>{p.catch(()=>{})});";
    check(os64_js_eval(runtime, source, os64_strlen(source), "handled.js", &outcome) == OS64_JS_OK && outcome.jobs_pending,
          "rejection deferred while jobs remain");
    check(os64_js_drain_jobs(runtime, 1, &outcome) == OS64_JS_MORE_JOBS && outcome.jobs_executed == 1,
          "intermediate slice is not rejection checkpoint");
    check(os64_js_drain_jobs(runtime, 20, &outcome) == OS64_JS_OK && !outcome.jobs_pending && outcome.jobs_executed == 2,
          "later handler suppresses rejection and preserves job count");
    check(execute_fixture(runtime, "new Promise(()=>{});", &outcome) == OS64_JS_OK && !outcome.jobs_pending,
          "unresolved Promise without runnable jobs completes");
    check(execute_fixture(runtime,
          "globalThis.converted=false;Promise.reject({toString(){converted=true;return 'bad'}});",
          &outcome) == OS64_JS_UNHANDLED_REJECTION, "object rejection uses safe diagnostic fallback");
    check(execute_fixture(runtime, "if(converted)throw Error('checkpoint executed user conversion')", &outcome) == OS64_JS_OK,
          "checkpoint diagnostic cannot create more work");
    os64_js_destroy(runtime);

    config.limits.jobs_per_turn = 2;
    runtime = create_fixture(&config);
    if (runtime == NULL) return;
    source = "function again(){Promise.resolve().then(again)};again()";
    check(os64_js_eval(runtime, source, os64_strlen(source), "chain", &outcome) == OS64_JS_OK && outcome.jobs_pending,
          "Promise chain begins one turn");
    check(os64_js_drain_jobs(runtime, 1, &outcome) == OS64_JS_MORE_JOBS && outcome.jobs_executed == 1,
          "first chain slice preserves turn");
    check(os64_js_drain_jobs(runtime, 1, &outcome) == OS64_JS_LIMIT &&
          outcome.limit == OS64_JS_LIMIT_JOBS && outcome.jobs_executed == 2,
          "job cap applies across slices at boundary");
    check(os64_js_eval(runtime, "true", 4, "after", &outcome) == OS64_JS_FAILED_RUNTIME,
          "limit retires runtime");
    os64_js_destroy(runtime);
}

static void limit_cases(void)
{
    os64_js_config_t config = fixture_config();
    os64_js_outcome_t outcome;
    config.limits.source_bytes = 4;
    os64_js_runtime_t *runtime = create_fixture(&config);
    if (runtime == NULL) return;
    check(execute_fixture(runtime, "true;", &outcome) == OS64_JS_LIMIT && outcome.limit == OS64_JS_LIMIT_SOURCE,
          "source limit retires runtime");
    os64_js_destroy(runtime);
    config = fixture_config();
    runtime = create_fixture(&config);
    if (runtime == NULL) return;
    unsigned before_finalization = finalized;
    check(install_class(runtime) >= 0, "native class installed before runtime failure");
    check(execute_fixture(runtime, "try{new Uint8Array(64*1024*1024)}catch(e){}", &outcome) == OS64_JS_LIMIT &&
          outcome.limit == OS64_JS_LIMIT_MEMORY, "memory refusal cannot become caught script success");
    os64_js_destroy(runtime);
    check(finalized == before_finalization + 1, "failed runtime still runs native finalizers");
    runtime = create_fixture(&config);
    if (runtime == NULL) return;
    fixture_runtime = runtime;
    check(install_native(runtime, "cancel", native_cancel) >= 0, "native cancellation installation");
    check(execute_fixture(runtime, "cancel();true", &outcome) == OS64_JS_CANCELLED,
          "callback cancellation is sticky");
    check(os64_js_eval(runtime, "true", 4, "after", &outcome) == OS64_JS_FAILED_RUNTIME,
          "cancelled runtime cannot be reused");
    os64_js_destroy(runtime); fixture_runtime = NULL;
    runtime = create_fixture(&config);
    if (runtime == NULL) return;
    os64_js_cancel(runtime);
    check(execute_fixture(runtime, "throw Error('must not run')", &outcome) == OS64_JS_CANCELLED,
          "pre-entry cancellation observed");
    os64_js_destroy(runtime);
}

static void runtime_cases(void)
{
    lifecycle_cases(); job_cases(); limit_cases();
}

static JSValue native_install(JSContext *context, JSValueConst self, int argc,
                               JSValueConst *args)
{
    (void)self; (void)argc; (void)args;
    os64_js_outcome_t nested;
    bool guarded = os64_js_install_args(fixture_runtime, 0, NULL, &nested) == OS64_JS_BUSY &&
        os64_js_install_output(fixture_runtime, 1, OS64_JS_OUTPUT_PRINT, &nested) == OS64_JS_BUSY &&
        os64_js_run_file(fixture_runtime, "nested.js", &nested) == OS64_JS_BUSY;
    return JS_NewBool(context, guarded);
}

/* The driver owns this output handle and verifies its returned bytes. */
static void helper_cases(int32_t handle)
{
    os64_js_config_t config = fixture_config();
    os64_js_runtime_t *runtime = create_fixture(&config);
    if (!runtime) return;
    os64_js_outcome_t outcome;
    const char *bad[] = {"ok", NULL};
    check(os64_js_install_args(runtime, 2, bad, &outcome) == OS64_JS_BAD_ARGUMENT &&
          os64_js_install_args(runtime, 1, NULL, &outcome) == OS64_JS_BAD_ARGUMENT &&
          os64_js_install_args(runtime, SIZE_MAX, bad, &outcome) == OS64_JS_BAD_ARGUMENT,
          "invalid arguments rejected before setup");
    check(os64_js_install_output(runtime, handle, 0, &outcome) == OS64_JS_BAD_ARGUMENT &&
          os64_js_install_output(runtime, handle, 4, &outcome) == OS64_JS_BAD_ARGUMENT &&
          os64_js_install_output(runtime, -1, OS64_JS_OUTPUT_PRINT, &outcome) == OS64_JS_BAD_ARGUMENT,
          "invalid output handle or names rejected");
    char mutable[] = "before";
    const char *args[] = {"script.js", "caf\xc3\xa9", mutable};
    check(os64_js_install_args(runtime, 3, args, &outcome) == OS64_JS_OK, "argument installer succeeds");
    mutable[0] = 'X';
    check(os64_js_install_args(runtime, 0, NULL, &outcome) == OS64_JS_BAD_ARGUMENT,
          "argument installer is installed once");
    check(os64_js_install_output(runtime, handle, OS64_JS_OUTPUT_PRINT | OS64_JS_OUTPUT_CONSOLE,
          &outcome) == OS64_JS_OK, "selected output names installed");
    check(os64_js_install_output(runtime, handle, OS64_JS_OUTPUT_PRINT, &outcome) == OS64_JS_BAD_ARGUMENT,
          "output installer is installed once");
    fixture_runtime = runtime;
    check(install_native(runtime, "setupGuarded", native_install) >= 0, "installer guard callback installed");
    check(execute_fixture(runtime,
          "if(scriptArgs.length!==3 || scriptArgs[0]!=='script.js' || scriptArgs[1]!=='caf\u00e9' ||"
          "scriptArgs[2]!=='before' || !setupGuarded()) throw Error('argument copy or reentry');"
          "print('hi',7,'a\\0b');console.log();console.log({toString(){return 'converted'}})",
          &outcome) == OS64_JS_OK, "copied UTF-8 arguments, reentry and output conversion");
    os64_js_destroy(runtime); fixture_runtime = NULL;

    runtime = create_fixture(&config);
    if (!runtime) return;
    JSContext *context = os64_js_context(runtime, OS64_JS_ABI_ID, &outcome);
    JSValue global = JS_GetGlobalObject(context), console = JS_NewObject(context);
    check(JS_SetPropertyStr(context, console, "keep", JS_NewInt32(context, 42)) >= 0 &&
          JS_SetPropertyStr(context, global, "console", console) >= 0,
          "host console with another member installed");
    JS_FreeValue(context, global);
    check(install_native(runtime, "print", native_twice) >= 0, "host print capability installed");
    check(os64_js_install_output(runtime, handle, OS64_JS_OUTPUT_CONSOLE, &outcome) == OS64_JS_OK &&
          os64_js_install_args(runtime, 0, NULL, &outcome) == OS64_JS_OK, "console-only and empty argument setup");
    check(execute_fixture(runtime,
          "if(print(21)!==42 || console.keep!==42 || scriptArgs.length!==0)throw Error('unselected name changed')",
          &outcome) == OS64_JS_OK, "unselected print and other console members preserved");
    os64_js_destroy(runtime);

    runtime = create_fixture(&config);
    if (!runtime) return;
    check(os64_js_install_output(runtime, handle, OS64_JS_OUTPUT_PRINT, &outcome) == OS64_JS_OK,
          "print-only setup");
    check(execute_fixture(runtime, "if(typeof console!=='undefined')throw Error('console granted')",
          &outcome) == OS64_JS_OK, "print-only leaves console absent");
    check(os64_js_install_args(runtime, 0, NULL, &outcome) == OS64_JS_BAD_ARGUMENT,
          "unused installer refused after evaluation");
    os64_js_destroy(runtime);
}

static unsigned transaction_getters;
static JSValue transaction_console_getter(JSContext *context, JSValueConst self,
                                          int argc, JSValueConst *args)
{
    (void)self; (void)argc; (void)args;
    transaction_getters++;
    return JS_NewObject(context);
}

static void output_transaction_cases(int32_t handle)
{
    os64_js_config_t config = fixture_config();
    const uint32_t both = OS64_JS_OUTPUT_PRINT | OS64_JS_OUTPUT_CONSOLE;
    for (unsigned mode = 0; mode < 5; mode++) {
        os64_js_runtime_t *runtime = create_fixture(&config);
        if (!runtime) return;
        os64_js_outcome_t outcome;
        JSContext *context = os64_js_context(runtime, OS64_JS_ABI_ID, &outcome);
        JSValue global = JS_GetGlobalObject(context), console = JS_NewObject(context);
        JSValue original = JS_NewCFunction(context, native_twice, "host print", 1);
        if (mode != 0 && mode != 4)
            check(JS_DefinePropertyValueStr(context, global, "print", JS_DupValue(context, original),
                  mode == 3 ? 0 : JS_PROP_CONFIGURABLE) > 0, "transaction original print installed");
        if (mode == 0 || mode == 1) {
            check(JS_DefinePropertyValueStr(context, global, "console", JS_NewInt32(context, 9),
                  JS_PROP_C_W_E) > 0, "transaction invalid console installed");
        } else if (mode == 4) {
            JSAtom name = JS_NewAtom(context, "console");
            check(JS_DefinePropertyGetSet(context, global, name,
                  JS_NewCFunction(context, transaction_console_getter, "get console", 0),
                  JS_UNDEFINED, JS_PROP_CONFIGURABLE) > 0, "transaction console accessor installed");
            JS_FreeAtom(context, name);
        } else {
            check(JS_DefinePropertyValueStr(context, console, "log", JS_NewInt32(context, 17),
                  mode == 2 ? 0 : JS_PROP_C_W_E) > 0 &&
                  JS_DefinePropertyValueStr(context, global, "console", JS_DupValue(context, console),
                  JS_PROP_C_W_E) > 0, "transaction original console log installed");
        }
        check(os64_js_install_output(runtime, handle, both, &outcome) == OS64_JS_EXCEPTION,
              "combined output failure is a script exception");
        JSAtom print_name = JS_NewAtom(context, "print");
        JSPropertyDescriptor descriptor = {0};
        int present = JS_GetOwnProperty(context, &descriptor, global, print_name);
        check(mode == 0 || mode == 4 ? present == 0 : present == 1 && JS_SameValue(context, descriptor.value, original) &&
              descriptor.flags == (mode == 3 ? 0 : JS_PROP_CONFIGURABLE),
              "combined failure restores print presence, identity and attributes");
        if (present > 0) {
            JS_FreeValue(context, descriptor.value); JS_FreeValue(context, descriptor.getter);
            JS_FreeValue(context, descriptor.setter);
        }
        JS_FreeAtom(context, print_name);
        if (mode == 2 || mode == 3) {
            JSValue log = JS_GetPropertyStr(context, console, "log");
            int32_t value = 0;
            check(JS_ToInt32(context, &value, log) == 0 && value == 17,
                  "combined failure leaves original console log unchanged");
            JS_FreeValue(context, log);
        }
        if (mode == 4) check(transaction_getters == 0, "transaction does not invoke console accessor");
        JSAtom console_name = JS_NewAtom(context, "console");
        check(JS_DeleteProperty(context, global, console_name, JS_PROP_THROW) > 0,
              "transaction fixture clears failed console target");
        JS_FreeAtom(context, console_name);
        check(os64_js_install_output(runtime, handle, OS64_JS_OUTPUT_CONSOLE, &outcome) == OS64_JS_OK,
              "console-only retry succeeds after combined exception");
        check(execute_fixture(runtime, mode == 0 || mode == 4 ? "if(typeof print!=='undefined')throw Error('print leaked')" :
              "if(print(21)!==42)throw Error('host print replaced')", &outcome) == OS64_JS_OK,
              "console-only retry does not retain an unselected print capability");
        JS_FreeValue(context, original); JS_FreeValue(context, console); JS_FreeValue(context, global);
        os64_js_destroy(runtime);
    }
}
#endif
