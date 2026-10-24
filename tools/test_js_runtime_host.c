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
#define JS_TEARDOWN_HOST
#include "../userland/libjs/tests/teardown.h"

static const char *input_bytes;
static size_t input_length, input_position, read_limit = SIZE_MAX;
static unsigned input_opens, input_closes;
static int64_t open_error, read_error, close_error, output_error;
static size_t read_fail_at = SIZE_MAX, output_fail_at = SIZE_MAX;
static int64_t loading_time;
static int64_t monotonic = 10000000, clock_step;
static os64_js_runtime_t *cancel_on_read;
static size_t cancel_read_at;

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
    size_t before = input_position;
    if (n > size) n = size;
    if (n > read_limit) n = read_limit;
    memcpy(buffer, input_bytes + input_position, n);
    input_position += n;
    monotonic += loading_time;
    if (cancel_on_read && before >= cancel_read_at) os64_js_cancel(cancel_on_read);
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

static void tracked_allocator_cases(void)
{
    JSPortAllocator owner = {.allocation_limit = 4096, .tracked = true};
    JSMallocState state = {.malloc_limit = SIZE_MAX, .opaque = &owner};
    const JSMallocFunctions *f = &jsport_tracked_malloc_functions;
    void *a = f->js_malloc(&state, 17);
    check(a != NULL && (uintptr_t)a % _Alignof(max_align_t) == 0 &&
          f->js_malloc_usable_size(a) >= 17 && owner.live_blocks == 1 &&
          owner.live_bytes == state.malloc_size, "tracked payload is aligned and metadata is charged");
    if (a == NULL) return;
    memset(a, 0x5a, 17);
    void *b = f->js_malloc(&state, 31);
    check(b != NULL && owner.live_blocks == 2, "ledger links a second block");
    size_t bytes = state.malloc_size;
    owner.allocation_limit = bytes;
    check(f->js_realloc(&state, a, 4096) == NULL && state.malloc_size == bytes &&
          owner.live_bytes == bytes && ((unsigned char *)a)[16] == 0x5a,
          "tracked over-budget growth preserves bytes links and charge");
    owner.allocation_limit = SIZE_MAX;
    check(f->js_malloc(&state, SIZE_MAX) == NULL && f->js_realloc(&state, a, SIZE_MAX) == NULL &&
          owner.live_bytes == bytes, "tracked header arithmetic refuses overflow");
    fail_at = attempts + 1;
    check(f->js_realloc(&state, a, 4096) == NULL && owner.live_bytes == bytes,
          "tracked allocation refusal preserves the ledger");
    fail_at = 0;
    a = f->js_realloc(&state, a, 257);
    check(a != NULL && ((unsigned char *)a)[16] == 0x5a && owner.live_blocks == 2 &&
          owner.live_bytes == state.malloc_size, "tracked resize replaces an interior ledger link");
    f->js_free(&state, b);
    check(owner.live_blocks == 1 && owner.live_bytes == state.malloc_size,
          "tracked free repairs neighboring links");
    check(f->js_realloc(&state, a, 0) == NULL && owner.live_blocks == 0 &&
          owner.live_bytes == 0 && state.malloc_size == 0, "tracked zero resize releases the ledger");
    void *raw = f->js_malloc(&state, 111);
    check(raw != NULL && owner.live_blocks == 1, "direct reclamation fixture allocated");
    jsport_allocator_reclaim(&owner);
    check(owner.blocks == NULL && owner.live_blocks == 0 && owner.live_bytes == 0 && live == 0,
          "ledger bulk reclaim frees storage without allocator state or engine entry");
}

static void tracked_construction_cases(void)
{
    os64_js_config_t config = fixture_config();
    os64_js_runtime_t *runtime = NULL;
    os64_js_outcome_t outcome;
    size_t begin = attempts;
    check(os64_js_create_with_teardown(&config, OS64_JS_TEARDOWN_RECLAIM,
          OS64_JS_ABI_ID, &runtime, &outcome) == OS64_JS_OK, "tracked constructor baseline");
    size_t inventory = attempts - begin;
    if (runtime == NULL) return;
    os64_js_destroy(runtime);
    for (size_t cut = 1; cut <= inventory; cut++) {
        fail_at = attempts + cut;
        os64_js_status_t result = os64_js_create_with_teardown(&config, OS64_JS_TEARDOWN_RECLAIM,
            OS64_JS_ABI_ID, &runtime, &outcome);
        fail_at = 0;
        check(result == OS64_JS_HOST_FAILURE && runtime == NULL && live == 0,
              "tracked constructor allocation refusal publishes no handle and reclaims its ledger");
        if (runtime != NULL) os64_js_destroy(runtime);
    }
    runtime = create_fixture(&config);
    if (runtime == NULL) return;
    size_t baseline = live;
    for (unsigned i = 0; i < 3; i++) {
        os64_js_runtime_t *leaking = NULL;
        check(os64_js_create_with_teardown(&config, OS64_JS_TEARDOWN_RECLAIM,
              OS64_JS_ABI_ID, &leaking, &outcome) == OS64_JS_OK, "peer leaking runtime constructed");
        if (leaking == NULL) break;
        JSContext *context = os64_js_context(leaking, OS64_JS_ABI_ID, &outcome);
        check(!JS_IsException(JS_NewObject(context)), "peer object deliberately lost");
        os64_js_teardown_report_t report;
        os64_js_destroy_report(leaking, &report);
        check(report.leaked && live == baseline && execute_fixture(runtime, "42", &outcome) == OS64_JS_OK,
              "reclamation leaves a separate runtime and its allocations usable");
    }
    os64_js_destroy(runtime);
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

/* Every allocation an eval of one script makes is failed in turn, on a
 * fresh raw engine: no cut crashes or asserts, each cut that lets the
 * script finish answers its value, each cut it cannot finish answers
 * exactly InternalError: "out of memory", and the engine's storage is all
 * reclaimed. Both probes run it (tools/test_js_runtime_host.sh): the
 * target core fails an arena at a time, the instrumented engine each
 * small allocation alone, which is what reaches the parser's paths. */
static void compile_failure_sweep(const char *name, const char *source)
{
    JSRuntime *engine = JS_NewRuntime();
    JSContext *context = engine != NULL ? JS_NewContext(engine) : NULL;
    if (context == NULL) {
        check(false, "compile failure: an engine to compile in");
        if (engine) JS_FreeRuntime(engine);
        return;
    }
    size_t length = strlen(source);
    size_t start = attempts;
    JSValue whole = JS_Eval(context, source, length, "compile.js", JS_EVAL_TYPE_GLOBAL);
    size_t count = attempts - start;
    int32_t expected = 0;
    check(!JS_IsException(whole) && JS_ToInt32(context, &expected, whole) == 0, "compile failure: the script runs whole");
    JS_FreeValue(context, whole);
    unsigned out_of_memory = 0, wrong = 0;
    for (size_t cut = 1; cut <= count; cut++) {
        fail_at = attempts + cut;
        JSValue value = JS_Eval(context, source, length, "compile.js", JS_EVAL_TYPE_GLOBAL);
        fail_at = 0;
        if (!JS_IsException(value)) {
            int32_t got = 0;
            if (JS_ToInt32(context, &got, value) != 0 || got != expected) {
                fprintf(stderr, "compile failure: %s cut %zu answered %d, not %d\n",
                        name, cut, (int)got, (int)expected);
                wrong++;
            }
            JS_FreeValue(context, value);
            continue;
        }
        JSValue error = JS_GetException(context);
        const char *text = JS_ToCString(context, error);
        if (text != NULL && strcmp(text, "InternalError: out of memory") == 0) out_of_memory++;
        else {
            fprintf(stderr, "compile failure: %s cut %zu answered %s\n", name, cut, text != NULL ? text : "(no text)");
            wrong++;
        }
        if (text) JS_FreeCString(context, text);
        JS_FreeValue(context, error);
    }
    printf("libjs compile failure: %s: %zu cuts, %u out of memory\n", name, count, out_of_memory);
    check(wrong == 0, "compile failure: each cut is the script's value or out of memory");
    check(out_of_memory > 0, "compile failure: the sweep reaches cuts that are out of memory");
    JS_FreeContext(context);
    JS_FreeRuntime(engine);
    check(live == 0, "compile failure: every cut's storage is reclaimed");
}

/* Patches 0010 and 0011a: an out-of-memory while a script is parsed and
 * made into functions is an exception through the paths they cover. Each
 * script is named for what its sweep reaches:
 * - nested: 0010's script (a child's bytecode emit cut short, read as
 *   another instruction); on the instrumented engine it also reaches a
 *   failed constant-pool add (upstream asserted), a failed closure-variable
 *   add for a local of an enclosing function (upstream's "stack
 *   underflow"), a failed resolve_scope_var read as the next position, and
 *   an emit failure get_lvalue named "invalid assignment left-hand side".
 * - eval_closure: a direct eval in strict code, whose compiled function
 *   reaches the eval's closure and a global through closure-variable adds.
 * - args_arrow, name_arrow: an arrow reading its enclosing function's
 *   `arguments` or name, whose failed add upstream captured as a wild
 *   argument slot (a SEGV).
 * - args_local, name_local: a function reading its own `arguments` or
 *   name, whose failed add upstream looked up as a global (a
 *   ReferenceError, or the wrong answer).
 * None has a jump, a with or a sloppy direct eval: each of those is
 * compiled through a label, and a label reaches resolve_labels, whose
 * output-buffer failure still crashes (DEBTS.md). */
static void compile_failure_cases(void)
{
    compile_failure_sweep("nested",
        "(function outer(seed){var total=seed;"
        "function inner(step){var a=step+1,b=a*2,c=b-3,d=c/4,e=d%5,f=e+total;"
        "return [a,b,c,d,e,f,'alpha','beta','gamma','delta'].length+f}"
        "return inner(1)+inner(2)+inner(3)+total})(1)");
    compile_failure_sweep("eval_closure",
        "(function(){'use strict';var v=41;return eval('(function(){return v+1})')()})()");
    compile_failure_sweep("args_arrow", "(function f(){return (()=>arguments.length)()})(1,2)");
    compile_failure_sweep("name_arrow", "(function h(){return (()=>typeof h)().length})()");
    compile_failure_sweep("args_local", "(function f(){return arguments.length})(1,2)");
    compile_failure_sweep("name_local", "(function g(){return (typeof g).length})()");
}

/* Sweeps that reach a parser out-of-memory path DEBTS.md still books, so
 * the suite does not run them; `--compile-failure-pending [name]` runs
 * them by hand, and each becomes a suite case when its path is paid.
 * - with, eval_var, eval_arg, eval_with: the closure-variable adds 0011a
 *   checks for a with object, a sloppy direct eval's variable object, its
 *   parameter-scope variable object, and the variables of code a direct
 *   eval compiles inside a with, plus the assignment through a with
 *   (OP_scope_make_ref). A with or a variable object is tested through a
 *   label, so each also reaches resolve_labels' failed output buffer.
 * - loop_arrow_class: a loop (resolve_labels' relocations written through
 *   a failed buffer, a SEGV), an arrow and a destructuring swap (the
 *   lookahead that swallows a failed atom, then a SyntaxError) and a class
 *   method's `this` (a failed pseudo-variable add, a wrong value). */
static void compile_failure_pending(const char *only)
{
    static const struct { const char *name, *source; } pending[] = {
        {"with", "(function(o){var t=0;with(o){(function(){t=x+1})()}return t})({x:41})"},
        {"eval_var", "var q=1;(function(){eval('var y=40');return function(){return y+q+1}})()()"},
        {"eval_arg", "var q=40;(function(a=eval('1'),f=function(){return a+q+1}){return f()})()"},
        {"eval_with", "(function(o){with(o){return eval('(function(){return w+1})')()}})({w:41})"},
        {"loop_arrow_class",
         "(function outer(seed){var total=seed;"
         "function inner(step){var a=step+1,b=a*2,c=b-3,d=c/4,e=d%5,f=e+total;"
         "return [a,b,c,d,e,f,'alpha','beta','gamma','delta'].length+f}"
         "var sum=(x,y)=>x+y,p=1,q=2;[p,q]=[q,p];"
         "for(var i=0;i<3;i++)total=sum(total,inner(i)+p);"
         "class Box{v=total;get(){return this.v}}"
         "return new Box().get()})(1)"},
    };
    for (size_t i = 0; i < sizeof(pending) / sizeof(pending[0]); i++)
        if (only == NULL || !strcmp(only, pending[i].name)) compile_failure_sweep(pending[i].name, pending[i].source);
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
    cancel_read_at = 0;
    output_len = 0; write_limit = SIZE_MAX;
}

static void file_boundary_cases(void)
{
    char source[8193];
    for (size_t capacity = 4096; capacity <= 8192; capacity *= 2) {
        os64_js_config_t config = file_boundary_config(capacity);
        os64_js_outcome_t outcome;
        for (size_t extra = 0; extra <= 1; extra++) {
            size_t length = capacity - 1 + extra;
            memset(source, ' ', length);
            memcpy(source, "true", 4);
            source[length] = 0;
            reset_io(source);
            os64_js_runtime_t *runtime = create_fixture(&config);
            if (!runtime) return;
            check(os64_js_run(runtime, source, length, "boundary.js", &outcome) == OS64_JS_OK,
                  "boundary source evaluates within the measured memory budget");
            os64_js_destroy(runtime);
            runtime = create_fixture(&config);
            if (!runtime) return;
            os64_js_status_t result = os64_js_run_file(runtime, "boundary.js", &outcome);
            check(result == OS64_JS_OK || (extra != 0 && result == OS64_JS_LIMIT &&
                  outcome.limit == OS64_JS_LIMIT_MEMORY),
                  "EOF boundary fits the budget and extra input respects the memory ceiling");
            check(input_closes == 1, "boundary load closes its input");
            os64_js_destroy(runtime);
            check(live == 0, "boundary fixtures release all allocations");
        }
    }
    for (unsigned mode = 0; mode < 4; mode++) {
        memset(source, ' ', 4096);
        source[0] = '(';
        memcpy(source + 4091, "true)", 5);
        source[mode == 2 ? 4095 : 4096] = 0;
        reset_io(source);
        os64_js_config_t config = mode == 3 ? fixture_config() : file_boundary_config(4096);
        os64_js_runtime_t *runtime = create_fixture(&config);
        if (!runtime) return;
        if (mode == 0) { read_fail_at = 4095; read_error = -45; close_error = OS64_CLOSE_DEFERRED; }
        if (mode == 1 || mode == 2) { cancel_on_read = runtime; cancel_read_at = 4095; }
        if (mode == 3) read_limit = 137;
        os64_js_outcome_t outcome;
        os64_js_status_t result = os64_js_run_file(runtime, "probe.js", &outcome);
        check(mode == 0 ? result == OS64_JS_HOST_FAILURE && outcome.host_error == -45 :
              mode < 3 ? result == OS64_JS_CANCELLED : result == OS64_JS_OK,
              "boundary probe preserves read error, cancellation and the extra source byte");
        check(input_closes == 1, "probe outcomes close the owned input");
        cancel_on_read = NULL;
        os64_js_destroy(runtime);
        check(live == 0, "probe outcomes release source and runtime allocations");
    }
    reset_io("");
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
        check(input_position == (mode == 0 ? 0 : 4096),
              "growth allocation refusal follows the confirmed extra input byte");
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

/* The Console Standard's namespace: levels print as log, assert says why,
 * groups indent, and counts and timers keep their labels. */
static void console_cases(void)
{
    os64_js_config_t config = fixture_config();
    os64_js_outcome_t outcome;
    reset_io("");
    os64_js_runtime_t *runtime = create_fixture(&config);
    if (!runtime) return;
    check(os64_js_install_output(runtime, 42, OS64_JS_OUTPUT_CONSOLE, &outcome) == OS64_JS_OK,
          "console namespace installed");
    check(execute_fixture(runtime,
          "console.info('i',1);console.warn('w');console.error('e');console.debug('d');"
          "console.trace('t');console.dir('o');console.table('r');"
          "console.assert(true,'quiet');console.assert(1===2,'sum is',3);console.assert(false);"
          "console.assert(0,{toString(){return 'obj'}});console.assert();"
          "console.group('g');console.log('in');console.groupCollapsed();console.log('deeper');"
          "console.groupEnd();console.groupEnd();console.groupEnd();console.log('out');"
          "console.group('h');console.clear();console.log('flat');"
          "console.count();console.count();console.count('x');console.countReset('x');console.count('x');"
          "console.countReset('nope');console.time('t');console.time('t');console.timeLog('nope');"
          "console.timeEnd('nope');"
          "for(const n of ['log','assert','groupCollapsed','timeEnd'])"
          "  if(console[n].name!==n||console[n].length!==0)throw Error('named '+n);"
          "console.timeLog('t','more');console.timeEnd('t');console.timeEnd('t')",
          &outcome) == OS64_JS_OK, "console methods run");
    const char expected[] =
        "i 1\nw\ne\nd\nt\no\nr\n"
        "Assertion failed: sum is 3\nAssertion failed\nAssertion failed obj\nAssertion failed\n"
        "g\n  in\n    deeper\nout\nh\nflat\n"
        "default: 1\ndefault: 2\nx: 1\nx: 1\nCount for 'nope' does not exist\n"
        "Timer 't' already exists\nTimer 'nope' does not exist\nTimer 'nope' does not exist\n";
    size_t n = sizeof(expected) - 1;
    check(output_len > n && memcmp(output, expected, n) == 0, "console lines as the standard prints them");
    /* Then `t: <ms>ms more`, `t: <ms>ms`, and the ended timer is gone. */
    char tail[256] = {0};
    memcpy(tail, output + n, output_len - n < sizeof(tail) - 1 ? output_len - n : sizeof(tail) - 1);
    char *second = strchr(tail, '\n');
    check(strncmp(tail, "t: ", 3) == 0 && second != NULL && strncmp(second - 7, "ms more", 7) == 0 &&
          strncmp(second + 1, "t: ", 3) == 0 && strstr(second + 1, "ms\nTimer 't' does not exist\n") != NULL,
          "console timers log, end, and forget");
    os64_js_destroy(runtime);
}

static void output_cases(void)
{
    os64_js_config_t config = fixture_config();
    os64_js_outcome_t outcome;
    reset_io(""); write_limit = 2;
    helper_cases(42);
    output_transaction_cases(42);
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

static void transaction_descriptor_cases(void)
{
    os64_js_config_t config = fixture_config();
    os64_js_outcome_t outcome;
    size_t inventory = 0;
    for (size_t refused = 0; refused <= inventory; refused++) {
        os64_js_runtime_t *runtime = create_fixture(&config);
        if (!runtime) return;
        JSContext *context = os64_js_context(runtime, OS64_JS_ABI_ID, &outcome);
        JSValue global = JS_GetGlobalObject(context), console = JS_NewObject(context);
        JSValue getter = JS_NewCFunction(context, console_getter, "old getter", 0);
        JSAtom print_name = JS_NewAtom(context, "print"), log_name = JS_NewAtom(context, "log");
        check(JS_DefinePropertyGetSet(context, global, print_name, JS_DupValue(context, getter),
              JS_UNDEFINED, JS_PROP_CONFIGURABLE) > 0 &&
              JS_DefinePropertyGetSet(context, console, log_name, JS_DupValue(context, getter),
              JS_UNDEFINED, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE) > 0 &&
              JS_SetPropertyStr(context, global, "console", JS_DupValue(context, console)) > 0,
              "transaction accessor descriptors prepared");
        size_t before = attempts;
        if (refused != 0) fail_at = attempts + refused;
        os64_js_status_t result = os64_js_install_output(runtime, 42,
            OS64_JS_OUTPUT_PRINT | OS64_JS_OUTPUT_CONSOLE, &outcome);
        fail_at = 0;
        if (refused == 0) {
            inventory = attempts - before;
            check(result == OS64_JS_OK, "accessor replacement allocation inventory succeeds");
        } else {
            check(result == OS64_JS_HOST_FAILURE, "accessor replacement allocation failure retires runtime");
            JSPropertyDescriptor print = {0}, log = {0};
            int print_present = JS_GetOwnProperty(context, &print, global, print_name);
            int log_present = JS_GetOwnProperty(context, &log, console, log_name);
            check(print_present == 1 && log_present == 1 &&
                  print.flags == (JS_PROP_CONFIGURABLE | JS_PROP_GETSET) &&
                  log.flags == (JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE | JS_PROP_GETSET) &&
                  JS_SameValue(context, print.getter, getter) && JS_SameValue(context, log.getter, getter),
                  "failed publication restores both original accessor identities and attributes");
            if (print_present > 0) {
                JS_FreeValue(context, print.value); JS_FreeValue(context, print.getter); JS_FreeValue(context, print.setter);
            }
            if (log_present > 0) {
                JS_FreeValue(context, log.value); JS_FreeValue(context, log.getter); JS_FreeValue(context, log.setter);
            }
        }
        JS_FreeAtom(context, print_name); JS_FreeAtom(context, log_name);
        JS_FreeValue(context, getter); JS_FreeValue(context, console); JS_FreeValue(context, global);
        os64_js_destroy(runtime);
        check(live == 0, "transaction descriptor sweep releases staged and original values");
    }
    check(console_getters == 0, "transaction descriptor sweep never invokes original getters");
}

static bool staged_output_blocked;
static int output_trap_define(JSContext *context, JSValueConst object, JSAtom name,
                              JSValueConst value, JSValueConst getter, JSValueConst setter, int flags)
{
    (void)object; (void)name; (void)getter; (void)setter; (void)flags;
    JSValue argument = JS_NewString(context, "must not write");
    JSValue called = JS_Call(context, value, JS_UNDEFINED, 1, &argument);
    staged_output_blocked = JS_IsException(called);
    if (staged_output_blocked) { JSValue error = JS_GetException(context); JS_FreeValue(context, error); }
    JS_FreeValue(context, called); JS_FreeValue(context, argument);
    JS_ThrowTypeError(context, "host definition trap refused");
    return -1;
}
static int output_trap_delete(JSContext *context, JSValueConst object, JSAtom name)
{
    (void)object; (void)name;
    JS_ThrowTypeError(context, "host rollback trap refused");
    return -1;
}
static void transaction_trap_case(void)
{
    os64_js_config_t config = fixture_config();
    os64_js_runtime_t *runtime = create_fixture(&config);
    if (!runtime) return;
    os64_js_outcome_t outcome;
    JSContext *context = os64_js_context(runtime, OS64_JS_ABI_ID, &outcome);
    static JSClassID slot;
    JSClassID id = os64_js_class_id(&slot);
    JSClassExoticMethods exotic = {.define_own_property = output_trap_define,
                                   .delete_property = output_trap_delete};
    const JSClassDef definition = {.class_name = "OutputTrapFixture", .exotic = &exotic};
    check(JS_NewClass(JS_GetRuntime(context), id, &definition) == 0, "transaction host trap class registered");
    JSValue global = JS_GetGlobalObject(context);
    check(JS_SetPropertyStr(context, global, "console", JS_NewObjectClass(context, id)) > 0,
          "transaction host console trap installed");
    size_t before = output_len;
    check(os64_js_install_output(runtime, 42, OS64_JS_OUTPUT_PRINT | OS64_JS_OUTPUT_CONSOLE,
          &outcome) == OS64_JS_HOST_FAILURE && staged_output_blocked && output_len == before,
          "rollback refusal retires runtime and staged callback cannot borrow output");
    check(os64_js_eval(runtime, "print('later')", 14, "after trap", &outcome) == OS64_JS_FAILED_RUNTIME,
          "rollback refusal does not expose a reusable partial capability");
    JS_FreeValue(context, global);
    os64_js_destroy(runtime);
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
            os64_js_install_output(runtime, 42, OS64_JS_OUTPUT_PRINT | OS64_JS_OUTPUT_CONSOLE, &outcome);
        size_t inventory = attempts - before;
        check(result == OS64_JS_OK, "installer allocation inventory succeeds");
        os64_js_destroy(runtime);
        for (size_t i = 1; i <= inventory; i++) {
            runtime = create_fixture(&config);
            if (!runtime) return;
            JSContext *inspection = mode == 1 ? os64_js_context(runtime, OS64_JS_ABI_ID, &outcome) : NULL;
            JSValue global = inspection ? JS_GetGlobalObject(inspection) : JS_UNDEFINED;
            fail_at = attempts + i;
            result = mode == 0 ? os64_js_install_args(runtime, 3, args, &outcome) :
                os64_js_install_output(runtime, 42, OS64_JS_OUTPUT_PRINT | OS64_JS_OUTPUT_CONSOLE, &outcome);
            fail_at = 0;
            check(result == OS64_JS_HOST_FAILURE, "installer allocation refusal is sticky host failure");
            if (mode == 1) {
                /* The context was borrowed before failure for native inspection;
                 * no script is executed through a retired wrapper. */
                JSValue print = JS_GetPropertyStr(inspection, global, "print");
                JSValue console = JS_GetPropertyStr(inspection, global, "console");
                check(JS_IsUndefined(print) && JS_IsUndefined(console),
                      "allocation refusal removes staged output capabilities");
                JS_FreeValue(inspection, print); JS_FreeValue(inspection, console);
                JS_FreeValue(inspection, global);
            }
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
    check(os64_js_install_output(runtime, 42, OS64_JS_OUTPUT_CONSOLE, &outcome) == OS64_JS_EXCEPTION &&
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
/* Host tasks (CONTRACT.md § Host tasks): a function value called under one
 * deadline and job cap that span every call and checkpoint of the task. */
/* Values are looked up and freed through a context borrowed while the runtime
 * was healthy: the accessor refuses a failed runtime, and these cases go on
 * calling into one to prove it answers FAILED_RUNTIME. */
static JSContext *task_context;
static JSValue task_global(os64_js_runtime_t *runtime, const char *name)
{
    (void)runtime;
    JSValue global = JS_GetGlobalObject(task_context);
    JSValue value = JS_GetPropertyStr(task_context, global, name);
    JS_FreeValue(task_context, global);
    return value;
}
static void task_free(os64_js_runtime_t *runtime, JSValue value)
{
    (void)runtime;
    JS_FreeValue(task_context, value);
}
static os64_js_status_t task_call(os64_js_runtime_t *runtime, const char *name, bool *returned_false,
                                  os64_js_outcome_t *outcome)
{
    JSValue function = task_global(runtime, name);
    os64_js_status_t result = os64_js_call(runtime, OS64_JS_ABI_ID, function, JS_UNDEFINED, 0, NULL,
                                           returned_false, outcome);
    check(result == outcome->status, "task call status matches outcome");
    task_free(runtime, function);
    return result;
}
static JSValue native_job_throws(JSContext *context, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    return JS_ThrowTypeError(context, "native job");
}
static JSValue native_enqueue_throw(JSContext *context, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self; (void)argc; (void)argv;
    return JS_EnqueueJob(context, native_job_throws, 0, NULL) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}
static JSValue native_task_nested(JSContext *context, JSValueConst self, int argc, JSValueConst *argv);
static os64_js_runtime_t *task_fixture(const os64_js_config_t *config, const char *source)
{
    os64_js_outcome_t outcome;
    os64_js_runtime_t *runtime = create_fixture(config);
    if (!runtime) return NULL;
    fixture_runtime = runtime;
    task_context = os64_js_context(runtime, OS64_JS_ABI_ID, &outcome);
    check(install_native(runtime, "nest", native_task_nested) >= 0 &&
          install_native(runtime, "enqueueThrow", native_enqueue_throw) >= 0 &&
          execute_fixture(runtime, source, &outcome) == OS64_JS_OK, "task fixture defined");
    return runtime;
}
static os64_js_status_t nested_call, nested_begin;
static JSValue native_task_nested(JSContext *context, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    os64_js_outcome_t outcome;
    nested_call = os64_js_call(fixture_runtime, OS64_JS_ABI_ID, argv[0], JS_UNDEFINED, 0, NULL, NULL, &outcome);
    nested_begin = os64_js_task_begin(fixture_runtime, OS64_JS_ABI_ID, "nested", &outcome);
    /* A binding's own dispatch nests with the raw call, inside the task. */
    return argc > 0 ? JS_Call(context, argv[0], JS_UNDEFINED, 0, NULL) : JS_UNDEFINED;
}
static void task_cases(void)
{
    os64_js_config_t config = fixture_config();
    os64_js_outcome_t outcome;
    os64_js_runtime_t *runtime = task_fixture(&config,
          "globalThis.log=[];globalThis.ok=()=>{log.push('ok')};globalThis.fails=()=>{throw Error('listener')};"
          "globalThis.no=()=>false;globalThis.zero=()=>0;"
          "globalThis.later=()=>{Promise.resolve().then(()=>log.push('job'))};"
          "globalThis.reject=()=>{Promise.reject(Error('lost'))};"
          "globalThis.jobs=()=>{for(let i=0;i<60;i++)Promise.resolve().then(()=>{})};"
          "globalThis.badJob=()=>{Promise.resolve().then(()=>{throw Error('job one')});"
          "Promise.resolve().then(()=>log.push('job two'))};"
          "globalThis.native=()=>{enqueueThrow();Promise.resolve().then(()=>log.push('native after'))};"
          "globalThis.spin=()=>{for(;;){}};globalThis.nested=()=>nest(spin);globalThis.reenter=()=>nest(ok);");
    if (!runtime) return;
    JSValue ok = task_global(runtime, "ok");
    check(os64_js_call(runtime, OS64_JS_ABI_ID, ok, JS_UNDEFINED, 0, NULL, NULL, &outcome) == OS64_JS_BAD_ARGUMENT,
          "call is refused outside a task");
    check(os64_js_checkpoint(runtime, OS64_JS_ABI_ID, &outcome) == OS64_JS_BAD_ARGUMENT &&
          os64_js_task_end(runtime, OS64_JS_ABI_ID, &outcome) == OS64_JS_BAD_ARGUMENT,
          "checkpoint and task end are refused outside a task");
    check(os64_js_task_begin(runtime, "other-abi", "abi", &outcome) == OS64_JS_ABI_MISMATCH,
          "task begin checks the caller's ABI");
    check(os64_js_task_begin(runtime, OS64_JS_ABI_ID, "task.js", &outcome) == OS64_JS_OK, "task opens");
    check(os64_js_task_begin(runtime, OS64_JS_ABI_ID, "second", &outcome) == OS64_JS_BUSY &&
          os64_js_eval(runtime, "true", 4, "inside", &outcome) == OS64_JS_BUSY &&
          os64_js_drain_jobs(runtime, 1, &outcome) == OS64_JS_BUSY,
          "an open task refuses a second task, eval and manual draining");
    bool returned_false = true;
    check(task_call(runtime, "fails", &returned_false, &outcome) == OS64_JS_EXCEPTION &&
          strstr(outcome.message, "listener") && !returned_false &&
          !strcmp(outcome.source_name, "task.js"), "a throwing callback reports its exception under the task name");
    check(task_call(runtime, "ok", &returned_false, &outcome) == OS64_JS_OK && !returned_false,
          "the next call in the same task still runs");
    check(task_call(runtime, "no", &returned_false, &outcome) == OS64_JS_OK && returned_false,
          "a strict false return is reported");
    check(task_call(runtime, "zero", &returned_false, &outcome) == OS64_JS_OK && !returned_false,
          "a falsy non-boolean return is not false");
    check(task_call(runtime, "reject", NULL, &outcome) == OS64_JS_OK,
          "a rejection is not judged by the call that made it");
    check(task_call(runtime, "later", NULL, &outcome) == OS64_JS_OK && outcome.jobs_pending,
          "a call leaves its jobs for the checkpoint");
    check(os64_js_checkpoint(runtime, OS64_JS_ABI_ID, &outcome) == OS64_JS_UNHANDLED_REJECTION &&
          strstr(outcome.message, "lost") && outcome.jobs_executed == 1 && !outcome.jobs_pending,
          "a checkpoint between calls runs the job and judges the rejection");
    check(os64_js_checkpoint(runtime, OS64_JS_ABI_ID, &outcome) == OS64_JS_OK,
          "a judged rejection is not reported twice");
    check(task_call(runtime, "badJob", NULL, &outcome) == OS64_JS_OK, "throwing job queued");
    check(os64_js_checkpoint(runtime, OS64_JS_ABI_ID, &outcome) == OS64_JS_UNHANDLED_REJECTION &&
          strstr(outcome.message, "job one") && !outcome.jobs_pending,
          "a Promise reaction that throws is a rejection, and the next job still runs");
    check(task_call(runtime, "native", NULL, &outcome) == OS64_JS_OK, "throwing native job queued");
    check(os64_js_checkpoint(runtime, OS64_JS_ABI_ID, &outcome) == OS64_JS_EXCEPTION &&
          strstr(outcome.message, "native job") && !outcome.jobs_pending,
          "a checkpoint reports the first job exception and drains the rest");
    check(task_call(runtime, "reenter", NULL, &outcome) == OS64_JS_OK &&
          nested_call == OS64_JS_BUSY && nested_begin == OS64_JS_BUSY,
          "call and task begin are refused from inside a callback");
    check(task_call(runtime, "reject", NULL, &outcome) == OS64_JS_OK, "rejection left for the end");
    check(os64_js_task_end(runtime, OS64_JS_ABI_ID, &outcome) == OS64_JS_UNHANDLED_REJECTION,
          "task end is a final checkpoint");
    check(execute_fixture(runtime, "if(log.join()!=='ok,job,job two,native after,ok')throw Error(log.join())", &outcome) == OS64_JS_OK,
          "a closed task accepts evaluation and every call ran once");
    check(os64_js_task_begin(runtime, OS64_JS_ABI_ID, "jobs", &outcome) == OS64_JS_OK, "job cap task opens");
    check(task_call(runtime, "jobs", NULL, &outcome) == OS64_JS_OK &&
          os64_js_checkpoint(runtime, OS64_JS_ABI_ID, &outcome) == OS64_JS_OK && outcome.jobs_executed == 60,
          "first checkpoint counts its jobs");
    check(task_call(runtime, "jobs", NULL, &outcome) == OS64_JS_OK &&
          os64_js_checkpoint(runtime, OS64_JS_ABI_ID, &outcome) == OS64_JS_LIMIT &&
          outcome.limit == OS64_JS_LIMIT_JOBS, "the job cap spans the task's checkpoints");
    check(os64_js_task_end(runtime, OS64_JS_ABI_ID, &outcome) == OS64_JS_LIMIT &&
          outcome.limit == OS64_JS_LIMIT_JOBS, "task end closes a failed task with its sticky status");
    check(os64_js_task_begin(runtime, OS64_JS_ABI_ID, "after", &outcome) == OS64_JS_FAILED_RUNTIME &&
          os64_js_eval(runtime, "true", 4, "after", &outcome) == OS64_JS_FAILED_RUNTIME,
          "nothing runs after a sticky task failure");
    task_free(runtime, ok);
    os64_js_destroy(runtime);

    config.limits.execution_ms = 1000;
    const char *deadline_source = "globalThis.ok=()=>{};globalThis.spin=()=>{for(;;){}};"
                                  "globalThis.nested=()=>nest(spin)";
    runtime = task_fixture(&config, deadline_source);
    if (!runtime) return;
    check(os64_js_set_execution_ms(runtime, 0, &outcome) == OS64_JS_BAD_ARGUMENT &&
          os64_js_set_execution_ms(runtime, (uint64_t)INT64_MAX, &outcome) == OS64_JS_BAD_ARGUMENT,
          "an execution limit must be positive and representable");
    check(os64_js_task_begin(runtime, OS64_JS_ABI_ID, "armed", &outcome) == OS64_JS_OK &&
          os64_js_set_execution_ms(runtime, 5, &outcome) == OS64_JS_OK, "limit changed inside an open task");
    monotonic += 100000;
    check(task_call(runtime, "ok", NULL, &outcome) == OS64_JS_OK &&
          os64_js_task_end(runtime, OS64_JS_ABI_ID, &outcome) == OS64_JS_OK,
          "the open task keeps the deadline it was armed with");
    check(os64_js_task_begin(runtime, OS64_JS_ABI_ID, "rearmed", &outcome) == OS64_JS_OK, "next task opens");
    monotonic += 100000;
    check(task_call(runtime, "ok", NULL, &outcome) == OS64_JS_LIMIT && outcome.limit == OS64_JS_LIMIT_EXECUTION,
          "the next task is armed with the new limit");
    os64_js_destroy(runtime);

    runtime = task_fixture(&config, deadline_source);
    if (!runtime) return;
    check(os64_js_task_begin(runtime, OS64_JS_ABI_ID, "overrun", &outcome) == OS64_JS_OK, "overrun task opens");
    clock_step = 1000;
    check(task_call(runtime, "nested", NULL, &outcome) == OS64_JS_LIMIT &&
          outcome.limit == OS64_JS_LIMIT_EXECUTION, "a binding's nested raw call is bounded by the task");
    clock_step = 0;
    check(task_call(runtime, "ok", NULL, &outcome) == OS64_JS_FAILED_RUNTIME &&
          os64_js_checkpoint(runtime, OS64_JS_ABI_ID, &outcome) == OS64_JS_FAILED_RUNTIME,
          "every later entry of the task answers failed");
    check(os64_js_task_end(runtime, OS64_JS_ABI_ID, &outcome) == OS64_JS_LIMIT &&
          outcome.limit == OS64_JS_LIMIT_EXECUTION && !strcmp(outcome.source_name, "overrun"),
          "task end reports the overrun");
    check(os64_js_task_end(runtime, OS64_JS_ABI_ID, &outcome) == OS64_JS_FAILED_RUNTIME,
          "a closed failed task is not closed twice");
    os64_js_destroy(runtime);
    fixture_runtime = NULL;
    task_context = NULL;
}
/* A script host (CONTRACT.md § A script host): every exception the wrapper
 * catches is handed to it, rejections wait for it to take them, and a
 * handler added to one it took is reported to it. */
static char host_heard[512];
static unsigned host_exceptions, host_handled;
static JSValue host_handled_promise;
static void host_note(JSContext *context, const char *what, JSValue value)
{
    const char *text = JS_ToCString(context, value);
    size_t at = strlen(host_heard);
    snprintf(host_heard + at, sizeof(host_heard) - at, "%s%s:%s", at ? "," : "", what, text ? text : "?");
    if (text) JS_FreeCString(context, text);
    else JS_FreeValue(context, JS_GetException(context));
}
static void host_exception(void *opaque, JSContext *context, JSValue error)
{
    check(opaque == &host_heard, "a script host hears its own opaque");
    host_exceptions++;
    host_note(context, "exception", error);
}
static void host_handled_hook(void *opaque, JSContext *context, JSValue promise)
{
    (void)opaque; (void)context;
    host_handled++;
    host_handled_promise = promise;
}
static bool host_value_is(JSValue value, const char *want)
{
    const char *text = JS_ToCString(task_context, value);
    bool same = text != NULL && !strcmp(text, want);
    if (text) JS_FreeCString(task_context, text);
    return same;
}
static JSValue native_take(JSContext *context, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self; (void)argc; (void)argv;
    JSValue promise, reason;
    bool taken = os64_js_take_rejection(fixture_runtime, OS64_JS_ABI_ID, &promise, &reason);
    return JS_NewBool(context, taken);
}
static void script_host_cases(void)
{
    os64_js_config_t config = fixture_config();
    config.limits.jobs_per_turn = 100000;   /* a catch is a job */
    os64_js_outcome_t outcome;
    os64_js_runtime_t *runtime = task_fixture(&config,
          "globalThis.rejected=[];globalThis.reject=n=>{for(let i=0;i<n;i++)rejected.push(Promise.reject('r'+i))};"
          "globalThis.handleAll=()=>{for(let i=rejected.length;i-->0;)rejected[i].catch(()=>{});rejected=[]};"
          "globalThis.fails=()=>{throw Error('called')};"
          "globalThis.jobThrows=()=>{queueJob()};"
          "globalThis.taken=null;globalThis.keep=()=>{taken=Promise.reject('kept')};"
          "globalThis.handleKept=()=>{taken.catch(()=>{})};");
    if (!runtime) return;
    check(install_native(runtime, "takeInside", native_take) >= 0 &&
          install_native(runtime, "queueJob", native_enqueue_throw) >= 0, "script host natives installed");
    JSValue promise, reason;
    check(!os64_js_take_rejection(runtime, OS64_JS_ABI_ID, &promise, &reason) &&
          JS_IsUndefined(promise) && JS_IsUndefined(reason), "nothing is taken without a script host");
    const os64_js_script_host_t host = {&host_heard, host_exception, host_handled_hook};
    check(os64_js_set_script_host(runtime, "other-abi", &host, &outcome) == OS64_JS_ABI_MISMATCH &&
          os64_js_set_script_host(runtime, OS64_JS_ABI_ID, &host, &outcome) == OS64_JS_OK, "a script host is set");
    check(execute_fixture(runtime, "throw Error('evaluated')", &outcome) == OS64_JS_EXCEPTION &&
          strstr(outcome.message, "evaluated") && !strcmp(host_heard, "exception:Error: evaluated"),
          "an evaluation's exception reaches the host and its outcome both");
    check(execute_fixture(runtime, "reject(3)", &outcome) == OS64_JS_OK,
          "with a script host a turn's rejections are not its outcome");
    check(os64_js_take_rejection(runtime, OS64_JS_ABI_ID, &promise, &reason) &&
          JS_IsObject(promise) && host_value_is(reason, "r0"), "the oldest rejection is taken first");
    task_free(runtime, promise);
    task_free(runtime, reason);
    check(execute_fixture(runtime, "handleAll()", &outcome) == OS64_JS_OK &&
          host_handled == 1 && !os64_js_take_rejection(runtime, OS64_JS_ABI_ID, &promise, &reason),
          "rejections handled before they were taken are gone; a taken one's handler reaches the host");
    check(execute_fixture(runtime, "reject(5000);handleAll()", &outcome) == OS64_JS_OK &&
          !os64_js_take_rejection(runtime, OS64_JS_ABI_ID, &promise, &reason),
          "five thousand rejections handled newest first leave nothing to take");
    check(execute_fixture(runtime, "reject(2);if(takeInside())throw Error('taken inside')", &outcome) == OS64_JS_OK,
          "nothing is taken from inside a call");
    check(os64_js_task_begin(runtime, OS64_JS_ABI_ID, "hosted", &outcome) == OS64_JS_OK &&
          task_call(runtime, "jobThrows", NULL, &outcome) == OS64_JS_OK && outcome.jobs_pending &&
          !os64_js_take_rejection(runtime, OS64_JS_ABI_ID, &promise, &reason),
          "nothing is taken while jobs are runnable");
    host_heard[0] = '\0';
    check(os64_js_checkpoint(runtime, OS64_JS_ABI_ID, &outcome) == OS64_JS_EXCEPTION &&
          !strcmp(host_heard, "exception:TypeError: native job"), "a checkpoint's job exception reaches the host");
    check(task_call(runtime, "fails", NULL, &outcome) == OS64_JS_EXCEPTION &&
          !strcmp(host_heard, "exception:TypeError: native job,exception:Error: called"),
          "a call's exception reaches the host");
    check(os64_js_set_script_host(runtime, OS64_JS_ABI_ID, &host, &outcome) == OS64_JS_OK,
          "a script host may be set between a task's calls");
    for (int i = 0; i < 2; i++) {
        check(os64_js_take_rejection(runtime, OS64_JS_ABI_ID, &promise, &reason) &&
              host_value_is(reason, i == 0 ? "r0" : "r1"), "the rest are taken in order after the checkpoint");
        task_free(runtime, promise);
        task_free(runtime, reason);
    }
    host_handled = 0;
    check(task_call(runtime, "keep", NULL, &outcome) == OS64_JS_OK &&
          os64_js_checkpoint(runtime, OS64_JS_ABI_ID, &outcome) == OS64_JS_OK &&
          os64_js_take_rejection(runtime, OS64_JS_ABI_ID, &promise, &reason),
          "a rejection is taken inside a task");
    check(task_call(runtime, "handleKept", NULL, &outcome) == OS64_JS_OK && host_handled == 1 &&
          JS_VALUE_GET_PTR(host_handled_promise) == JS_VALUE_GET_PTR(promise),
          "a handler added to it later names it to the host");
    task_free(runtime, promise);
    task_free(runtime, reason);
    check(os64_js_task_end(runtime, OS64_JS_ABI_ID, &outcome) == OS64_JS_OK, "the hosted task closes clean");
    check(os64_js_set_script_host(runtime, OS64_JS_ABI_ID, NULL, &outcome) == OS64_JS_OK &&
          execute_fixture(runtime, "reject(1)", &outcome) == OS64_JS_UNHANDLED_REJECTION,
          "a cleared host gives the checkpoint its rejections back");
    check(os64_js_set_script_host(runtime, OS64_JS_ABI_ID, &host, &outcome) == OS64_JS_OK &&
          execute_fixture(runtime, "reject(1)", &outcome) == OS64_JS_OK, "left for a host at destruction");
    os64_js_destroy(runtime);
    runtime = task_fixture(&config, "globalThis.spin=()=>{for(;;){}};");
    if (!runtime) return;
    check(os64_js_set_script_host(runtime, OS64_JS_ABI_ID, &host, &outcome) == OS64_JS_OK, "second host set");
    host_heard[0] = '\0';
    host_exceptions = 0;
    clock_step = 1000000;
    check(execute_fixture(runtime, "spin()", &outcome) == OS64_JS_LIMIT && host_exceptions == 0,
          "an exception a sticky failure threw is not the host's");
    clock_step = 0;
    check(os64_js_set_script_host(runtime, OS64_JS_ABI_ID, NULL, &outcome) == OS64_JS_OK,
          "a host is cleared from a failed runtime");
    os64_js_destroy(runtime);
    fixture_runtime = NULL;
    task_context = NULL;
}
int main(int argc, char **argv)
{
    if ((argc == 2 && !strcmp(argv[1], "--compile-failure")) ||
        ((argc == 2 || argc == 3) && !strcmp(argv[1], "--compile-failure-pending"))) {
        if (!strcmp(argv[1], "--compile-failure")) compile_failure_cases();
        else compile_failure_pending(argc == 3 ? argv[2] : NULL);
        printf("libjs compile failure: %u checks, %u failures\n", checks, failures);
        return failures != 0;
    }
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
        if (!strncmp(argv[1], "--reclaim-", 10)) {
            os64_js_outcome_t outcome;
            os64_js_create_with_teardown(&config, OS64_JS_TEARDOWN_RECLAIM, OS64_JS_ABI_ID,
                                         &fixture_runtime, &outcome);
        } else fixture_runtime = create_fixture(&config);
        if (!fixture_runtime) return 1;
        if (!strcmp(argv[1], "--leak")) {
            os64_js_outcome_t outcome;
            JSContext *context = os64_js_context(fixture_runtime, OS64_JS_ABI_ID, &outcome);
            (void)JS_NewObject(context);
            os64_js_destroy(fixture_runtime);
        } else if (!strcmp(argv[1], "--reclaim-raw-leak")) {
            os64_js_outcome_t outcome;
            JSContext *context = os64_js_context(fixture_runtime, OS64_JS_ABI_ID, &outcome);
            (void)JS_NewObject(context);
            JSRuntime *engine = JS_GetRuntime(context);
            JS_FreeContext(context);
            JS_FreeRuntime(engine);
        } else if (!strcmp(argv[1], "--reclaim-fatal")) {
            jsport_fatal("unrelated invariant fixture", __FILE__, __LINE__);
        } else if (!strcmp(argv[1], "--active-destroy") || !strcmp(argv[1], "--reclaim-active-destroy")) {
            os64_js_outcome_t outcome;
            install_native(fixture_runtime, "destroy", native_destroy);
            execute_fixture(fixture_runtime, "destroy()", &outcome);
        }
        return 1;
    }
    tracked_allocator_cases(); tracked_construction_cases(); teardown_cases(); runtime_cases(); clock_cases(); diagnostic_cases(); allocation_cases(); thread_cases();
    file_cases(); file_boundary_cases(); output_cases(); console_cases(); transaction_descriptor_cases(); transaction_trap_case(); setup_allocation_cases();
    task_cases();
    script_host_cases();
    check(live == 0, "runtime suite releases every fixture allocation");
    printf("libjs runtime: %u checks, %u failures, %zu live allocations, %u native calls\n",
           checks, failures, live, native_calls);
    return failures != 0;
}
