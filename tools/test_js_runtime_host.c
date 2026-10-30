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

/* Patch 0013: a prototype set whose shape copy is refused gives back the
 * reference it took on the new prototype. Two fresh objects share their
 * shape, so setting one's prototype must copy it. The set's allocations
 * are counted on a first run and each refused in turn (as the compile
 * sweep does: the target core's arenas may make none to count, the
 * instrumented engine makes the copy's own), and after every one the
 * runtime's teardown finds nothing left. */
static void set_prototype_refused_cases(void)
{
    os64_js_config_t config = fixture_config();
    size_t count = 0;
    {
        os64_js_runtime_t *runtime = NULL;
        os64_js_outcome_t outcome;
        if (os64_js_create_with_teardown(&config, OS64_JS_TEARDOWN_RECLAIM, OS64_JS_ABI_ID,
                                         &runtime, &outcome) != OS64_JS_OK || runtime == NULL) {
            check(false, "prototype set fixture constructed");
            return;
        }
        JSContext *context = os64_js_context(runtime, OS64_JS_ABI_ID, &outcome);
        JSValue proto = JS_NewObject(context), object = JS_NewObject(context), sibling = JS_NewObject(context);
        size_t start = attempts;
        check(JS_SetPrototype(context, object, proto) == 1, "an unrefused prototype set succeeds");
        count = attempts - start;
        JS_FreeValue(context, sibling);
        JS_FreeValue(context, object);
        JS_FreeValue(context, proto);
        os64_js_teardown_report_t report;
        os64_js_destroy_report(runtime, &report);
        check(!report.leaked, "an unrefused prototype set leaves nothing for teardown");
    }
    unsigned refused = 0;
    for (size_t cut = 1; cut <= count; cut++) {
        os64_js_runtime_t *runtime = NULL;
        os64_js_outcome_t outcome;
        if (os64_js_create_with_teardown(&config, OS64_JS_TEARDOWN_RECLAIM, OS64_JS_ABI_ID,
                                         &runtime, &outcome) != OS64_JS_OK || runtime == NULL) {
            check(false, "prototype set fixture constructed");
            return;
        }
        JSContext *context = os64_js_context(runtime, OS64_JS_ABI_ID, &outcome);
        JSValue proto = JS_NewObject(context), object = JS_NewObject(context), sibling = JS_NewObject(context);
        fail_at = attempts + cut;
        int set = JS_SetPrototype(context, object, proto);
        fail_at = 0;
        if (set < 0) {
            refused++;
            JS_FreeValue(context, JS_GetException(context));
        }
        JS_FreeValue(context, sibling);
        JS_FreeValue(context, object);
        JS_FreeValue(context, proto);
        os64_js_teardown_report_t report;
        os64_js_destroy_report(runtime, &report);
        check(!report.leaked, "a prototype set, refused or not, leaves nothing for teardown to find");
    }
    check(refused == count, "each of the set's allocations, refused, refuses the set");
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

/* Patch 0014: the v flag's ClassSetCharacter rules hold inside a class and
 * a class string, and nowhere else: an atom outside a class may be a slash,
 * a hyphen or a doubled punctuator, as it may be with the u flag. */
static void regexp_unicode_sets_cases(void)
{
    os64_js_config_t config = fixture_config();
    os64_js_runtime_t *runtime = create_fixture(&config);
    if (!runtime) return;
    os64_js_outcome_t outcome;
    check(execute_fixture(runtime,
          "if(!new RegExp('^(?:http://a-b.example)$','v').test('http://a-b.example'))throw Error('slash, hyphen');"
          "if(!/a::b@@c/v.test('a::b@@c'))throw Error('doubled punctuators outside a class');"
          "if(new RegExp('^(?:a/b)$','v').test('a-b'))throw Error('the slash is a character');", &outcome) == OS64_JS_OK,
          "a v-flag atom outside a class may be a slash, a hyphen or a doubled punctuator");
    static const char *const refused[] = {"/[a-]/v", "/[(]/v", "/[a&&&b]/v", "/[\\q{a/b}]/v", "/[\\q{a::b}]/v"};
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++)
        check(execute_fixture(runtime, refused[i], &outcome) == OS64_JS_EXCEPTION, "a v-flag class keeps its rules");
    check(execute_fixture(runtime, "if(!/[\\q{a\\-b}]/v.test('a-b'))throw Error('escaped')", &outcome) == OS64_JS_OK,
          "a class string's escaped hyphen is a class atom's");
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
static bool nested_turn_open;
static JSValue native_task_nested(JSContext *context, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    os64_js_outcome_t outcome;
    nested_turn_open = os64_js_turn_open(fixture_runtime);
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
    check(!os64_js_turn_open(runtime) && !os64_js_turn_open(NULL),
          "no turn is open between tasks: a job queued now would be drained by nobody");
    check(os64_js_task_begin(runtime, OS64_JS_ABI_ID, "task.js", &outcome) == OS64_JS_OK, "task opens");
    check(os64_js_turn_open(runtime), "an open task is a turn its jobs are drained in");
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
    check(nested_turn_open, "a callback runs inside the turn");
    check(os64_js_task_end(runtime, OS64_JS_ABI_ID, &outcome) == OS64_JS_UNHANDLED_REJECTION,
          "task end is a final checkpoint");
    check(!os64_js_turn_open(runtime), "a closed task's turn is over");
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
/* The script on the stack as a promise is rejected: 7 for the fixture's
 * own evaluation, which defines `reject`, else 1. */
static uint32_t host_rejected_hook(void *opaque, JSContext *context, JSValue promise)
{
    (void)opaque; (void)promise;
    uint32_t answer = 1;
    for (int n = 0; n < 8 && answer == 1; n++) {
        JSAtom name = JS_GetScriptOrModuleName(context, n);
        if (name == JS_ATOM_NULL) continue;
        const char *text = JS_AtomToCString(context, name);
        if (text != NULL && !strcmp(text, "runtime-fixture.js")) answer = 7;
        if (text != NULL) JS_FreeCString(context, text);
        JS_FreeAtom(context, name);
    }
    return answer;
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
    bool taken = os64_js_take_rejection(fixture_runtime, OS64_JS_ABI_ID, &promise, &reason, NULL);
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
    check(!os64_js_take_rejection(runtime, OS64_JS_ABI_ID, &promise, &reason, NULL) &&
          JS_IsUndefined(promise) && JS_IsUndefined(reason), "nothing is taken without a script host");
    const os64_js_script_host_t host = {&host_heard, host_exception, host_handled_hook, host_rejected_hook};
    check(os64_js_set_script_host(runtime, "other-abi", &host, &outcome) == OS64_JS_ABI_MISMATCH &&
          os64_js_set_script_host(runtime, OS64_JS_ABI_ID, &host, &outcome) == OS64_JS_OK, "a script host is set");
    check(execute_fixture(runtime, "throw Error('evaluated')", &outcome) == OS64_JS_EXCEPTION &&
          strstr(outcome.message, "evaluated") && !strcmp(host_heard, "exception:Error: evaluated"),
          "an evaluation's exception reaches the host and its outcome both");
    check(execute_fixture(runtime, "reject(3)", &outcome) == OS64_JS_OK,
          "with a script host a turn's rejections are not its outcome");
    uint32_t tag = 0;
    check(os64_js_take_rejection(runtime, OS64_JS_ABI_ID, &promise, &reason, &tag) &&
          JS_IsObject(promise) && host_value_is(reason, "r0"), "the oldest rejection is taken first");
    check(tag == 7, "with the host's answer, asked while the script that rejected was on the stack");
    task_free(runtime, promise);
    task_free(runtime, reason);
    check(execute_fixture(runtime, "handleAll()", &outcome) == OS64_JS_OK &&
          host_handled == 1 && !os64_js_take_rejection(runtime, OS64_JS_ABI_ID, &promise, &reason, NULL),
          "rejections handled before they were taken are gone; a taken one's handler reaches the host");
    check(execute_fixture(runtime, "reject(5000);handleAll()", &outcome) == OS64_JS_OK &&
          !os64_js_take_rejection(runtime, OS64_JS_ABI_ID, &promise, &reason, NULL),
          "five thousand rejections handled newest first leave nothing to take");
    check(execute_fixture(runtime, "reject(2);if(takeInside())throw Error('taken inside')", &outcome) == OS64_JS_OK,
          "nothing is taken from inside a call");
    check(os64_js_task_begin(runtime, OS64_JS_ABI_ID, "hosted", &outcome) == OS64_JS_OK &&
          task_call(runtime, "jobThrows", NULL, &outcome) == OS64_JS_OK && outcome.jobs_pending &&
          !os64_js_take_rejection(runtime, OS64_JS_ABI_ID, &promise, &reason, NULL),
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
        check(os64_js_take_rejection(runtime, OS64_JS_ABI_ID, &promise, &reason, NULL) &&
              host_value_is(reason, i == 0 ? "r0" : "r1"), "the rest are taken in order after the checkpoint");
        task_free(runtime, promise);
        task_free(runtime, reason);
    }
    host_handled = 0;
    check(task_call(runtime, "keep", NULL, &outcome) == OS64_JS_OK &&
          os64_js_checkpoint(runtime, OS64_JS_ABI_ID, &outcome) == OS64_JS_OK &&
          os64_js_take_rejection(runtime, OS64_JS_ABI_ID, &promise, &reason, NULL),
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
/* ES modules (CONTRACT.md § Modules): a graph compiled unresolved and run
 * once every module is compiled, the loader for what was not compiled, and
 * a module's failure as its own outcome, never an unhandled rejection. */
static const char *const module_store[][2] = {
    {"https://m.test/a.js", "import {b} from './b.js'; export const a = 'a' + b; globalThis.order += 'a';"},
    {"https://m.test/b.js", "export const b = 'b'; globalThis.order += 'b';"},
    {"https://m.test/c1.js", "import {c2} from './c2.js'; export const c1 = 'c1'; globalThis.order += 'c1' + c2;"},
    {"https://m.test/c2.js", "import {c1} from './c1.js'; export const c2 = 'c2'; globalThis.order += 'c2';"},
    {"https://m.test/lazy.js", "export default 'lazy';"},
    {"https://m.test/dyn.js", "export default 'dyn';"},
    {"https://m.test/bad.js", "export const x = ;"},
    /* The loader answers for every module it knows, as a host's map does:
     * a failed run unlists the modules no run resolved (CONTRACT.md). */
    {"https://m.test/shared.js", "import './b.js'; import {late} from './late.js'; globalThis.shared = late;"},
    {"https://m.test/first.js", "import './shared.js';"},
    {"https://m.test/second.js", "import './shared.js';"},
    /* Cycles whose entry's own import fails: every member points back at it. */
    {"https://m.test/cyc-e.js", "import './cyc-a.js'; import 'bare';"},
    {"https://m.test/cyc-a.js", "import './cyc-e.js';"},
    {"https://m.test/cyc-g.js", "import './cyc-a.js';"},
    {"https://m.test/dyn-x.js", "import './dyn-y.js'; import 'bare';"},
    {"https://m.test/dyn-y.js", "import './dyn-x.js';"},
};
static unsigned module_loads;
static char module_requests[256];
static const char *module_late;     /* late.js's source, once it is there */
static char *module_resolve(void *opaque, const char *base, const char *specifier)
{
    check(opaque == module_store, "the loader hears its own opaque");
    if (strncmp(specifier, "./", 2) != 0) return NULL;      /* a bare name names nothing here */
    const char *slash = strrchr(base, '/');
    size_t keep = slash != NULL ? (size_t)(slash - base + 1) : 0;
    char *name = os64_malloc(keep + strlen(specifier + 2) + 1);
    if (name == NULL) return NULL;
    memcpy(name, base, keep);
    strcpy(name + keep, specifier + 2);
    return name;
}
static const char *module_text(const char *name)
{
    if (!strcmp(name, "https://m.test/late.js")) return module_late;
    for (size_t i = 0; i < sizeof(module_store) / sizeof(module_store[0]); i++)
        if (!strcmp(name, module_store[i][0])) return module_store[i][1];
    return NULL;
}
static bool module_source(void *opaque, const char *name, const char **bytes, size_t *length)
{
    (void)opaque;
    module_loads++;
    *bytes = module_text(name);
    *length = *bytes != NULL ? strlen(*bytes) : 0;
    return *bytes != NULL;
}
static void module_request(void *opaque, const char *specifier)
{
    (void)opaque;
    size_t at = strlen(module_requests);
    snprintf(module_requests + at, sizeof(module_requests) - at, "%s%s", at ? "," : "", specifier);
}
static os64_js_module_t *module_compile(os64_js_runtime_t *runtime, const char *name, const char *source,
                                        os64_js_outcome_t *outcome)
{
    if (source == NULL) source = module_text(name);
    os64_js_module_t *module = NULL;
    os64_js_status_t result = os64_js_module_compile(runtime, OS64_JS_ABI_ID, source, strlen(source), name,
                                                     NULL, &module, outcome);
    check(result == outcome->status, "module compile status matches outcome");
    return module;
}
/* One task around a module's run, as a browser's script task is. */
static os64_js_status_t module_task(os64_js_runtime_t *runtime, os64_js_module_t *module, os64_js_outcome_t *outcome)
{
    os64_js_outcome_t closing;
    if (os64_js_task_begin(runtime, OS64_JS_ABI_ID, "module", outcome) != OS64_JS_OK) return outcome->status;
    os64_js_status_t result = os64_js_module_run(runtime, OS64_JS_ABI_ID, module, outcome);
    os64_js_task_end(runtime, OS64_JS_ABI_ID, &closing);
    return result;
}
static void module_cases(void)
{
    os64_js_config_t config = fixture_config();
    os64_js_outcome_t outcome;
    os64_js_runtime_t *runtime = task_fixture(&config, "globalThis.order = '';");
    if (!runtime) return;
    const os64_js_module_loader_t loader = {(void *)module_store, module_resolve, module_source};
    check(os64_js_set_module_loader(runtime, "other-abi", &loader, &outcome) == OS64_JS_ABI_MISMATCH &&
          os64_js_set_module_loader(runtime, OS64_JS_ABI_ID, &loader, &outcome) == OS64_JS_OK, "a module loader is set");
    JSValue raw = JS_Eval(task_context, "1", 1, "raw", JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_NO_RESOLVE);
    check(JS_IsException(raw), "the engine refuses NO_RESOLVE outside a compiled-only module");
    JS_FreeValue(task_context, JS_GetException(task_context));

    // A graph: each module compiled as its source arrives, then run.
    os64_js_module_t *a = module_compile(runtime, "https://m.test/a.js", NULL, &outcome);
    check(a != NULL && outcome.status == OS64_JS_OK, "an entry compiles unresolved, its import not there yet");
    module_requests[0] = '\0';
    check(os64_js_module_requests(runtime, OS64_JS_ABI_ID, a, module_request, NULL, &outcome) == OS64_JS_OK &&
          !strcmp(module_requests, "./b.js"), "its request is named before it is resolved");
    os64_js_module_t *b = module_compile(runtime, "https://m.test/b.js", NULL, &outcome);
    check(os64_js_module_state(runtime, a, NULL) == OS64_JS_MODULE_UNRUN, "a compiled module is unrun");
    check(os64_js_module_run(runtime, OS64_JS_ABI_ID, a, &outcome) == OS64_JS_BAD_ARGUMENT,
          "a module runs only inside a task");
    module_loads = 0;
    check(module_task(runtime, a, &outcome) == OS64_JS_OK && os64_js_module_state(runtime, a, NULL) == OS64_JS_MODULE_FULFILLED,
          "the graph runs once every module is compiled");
    JSValue order = task_global(runtime, "order");
    check(host_value_is(order, "ba") && module_loads == 0, "its import first, found by name, no load asked");
    task_free(runtime, order);
    check(module_task(runtime, a, &outcome) == OS64_JS_BAD_ARGUMENT, "a module is run once");

    // An inline module: a name of its own, the document's URL for import.meta.
    const char *inline_source = "import {b} from './b.js'; globalThis.meta = import.meta.url; globalThis.inlineB = b;";
    os64_js_module_t *inline_module = NULL;
    check(os64_js_module_compile(runtime, OS64_JS_ABI_ID, inline_source, strlen(inline_source),
                                 "https://m.test/page#inline-1", "https://m.test/page", &inline_module, &outcome) == OS64_JS_OK &&
          module_task(runtime, inline_module, &outcome) == OS64_JS_OK, "an inline module runs");
    JSValue meta = task_global(runtime, "meta"), inline_b = task_global(runtime, "inlineB");
    order = task_global(runtime, "order");
    check(host_value_is(meta, "https://m.test/page") && host_value_is(inline_b, "b") && host_value_is(order, "ba"),
          "import.meta.url is the URL it was given, and b is not evaluated again");
    task_free(runtime, meta); task_free(runtime, inline_b); task_free(runtime, order);

    // A cycle links, each module once, the one first imported finishing first.
    os64_js_module_t *c1 = module_compile(runtime, "https://m.test/c1.js", NULL, &outcome);
    os64_js_module_t *c2 = module_compile(runtime, "https://m.test/c2.js", NULL, &outcome);
    check(module_task(runtime, c1, &outcome) == OS64_JS_OK && os64_js_module_state(runtime, c1, NULL) == OS64_JS_MODULE_FULFILLED,
          "a cycle links and runs");
    order = task_global(runtime, "order");
    check(host_value_is(order, "bac2c1c2"), "its modules once each, the imported one first");
    task_free(runtime, order);

    // What was not compiled is the loader's, and a dynamic import() too.
    os64_js_module_t *lazy = module_compile(runtime, "https://m.test/entry.js",
        "import lazy from './lazy.js'; globalThis.lazy = lazy; import('./dyn.js').then(m => globalThis.dyn = m.default);",
        &outcome);
    module_loads = 0;
    check(module_task(runtime, lazy, &outcome) == OS64_JS_OK && module_loads == 2 &&
          os64_js_module_state(runtime, lazy, NULL) == OS64_JS_MODULE_FULFILLED,
          "a module nobody compiled is loaded, and import() is answered inside the task");
    JSValue lazy_value = task_global(runtime, "lazy"), dyn_value = task_global(runtime, "dyn");
    check(host_value_is(lazy_value, "lazy") && host_value_is(dyn_value, "dyn"), "both are what the loader answered");
    task_free(runtime, lazy_value); task_free(runtime, dyn_value);

    // The request list, in source order, export-from included.
    os64_js_module_t *asks = module_compile(runtime, "https://m.test/asks.js",
        "import './b.js'; export * from './c2.js'; import {q} from './q.js';", &outcome);
    module_requests[0] = '\0';
    check(os64_js_module_requests(runtime, OS64_JS_ABI_ID, asks, module_request, NULL, &outcome) == OS64_JS_OK &&
          !strcmp(module_requests, "./b.js,./c2.js,./q.js"), "requests in source order, export-from among them");

    // Failures, each the module's own.
    const os64_js_script_host_t host = {&host_heard, host_exception, host_handled_hook, host_rejected_hook};
    check(os64_js_set_script_host(runtime, OS64_JS_ABI_ID, &host, &outcome) == OS64_JS_OK, "module script host set");
    host_heard[0] = '\0';
    check(module_compile(runtime, "https://m.test/bad.js", NULL, &outcome) == NULL && outcome.status == OS64_JS_EXCEPTION &&
          strstr(outcome.message, "SyntaxError") != NULL && host_heard[0] == '\0',
          "a syntax error is the compile's exception alone, never the script host's");
    check(module_task(runtime, asks, &outcome) == OS64_JS_EXCEPTION && strstr(outcome.message, "q.js") != NULL &&
          os64_js_module_state(runtime, asks, NULL) == OS64_JS_MODULE_REJECTED, "an import nobody can load fails the run");
    os64_js_module_t *nope = module_compile(runtime, "https://m.test/nope.js", "import {nope} from './b.js';", &outcome);
    check(module_task(runtime, nope, &outcome) == OS64_JS_EXCEPTION && strstr(outcome.message, "nope") != NULL,
          "an export that is not there fails the link");
    host_heard[0] = '\0';
    os64_js_module_t *boom = module_compile(runtime, "https://m.test/boom.js", "throw Error('boom');", &outcome);
    os64_js_outcome_t failure;
    JSValue promise, reason;
    check(module_task(runtime, boom, &outcome) == OS64_JS_EXCEPTION && strstr(outcome.message, "boom") != NULL &&
          os64_js_module_state(runtime, boom, &failure) == OS64_JS_MODULE_REJECTED && strstr(failure.message, "boom") != NULL,
          "an evaluation that throws is the run's exception and the module's failure");
    check(!strcmp(host_heard, "exception:Error: boom") &&
          !os64_js_take_rejection(runtime, OS64_JS_ABI_ID, &promise, &reason, NULL),
          "reported to the host once as an exception, never as an unhandled rejection");

    // A top-level await: pending across turns, settled when what it awaits is.
    os64_js_module_t *waits = module_compile(runtime, "https://m.test/waits.js",
        "await new Promise(r => globalThis.release = r); globalThis.after = 'after';", &outcome);
    check(module_task(runtime, waits, &outcome) == OS64_JS_OK && os64_js_module_state(runtime, waits, NULL) == OS64_JS_MODULE_PENDING,
          "an awaiting module is pending after its task, its jobs drained");
    os64_js_outcome_t closing;
    check(os64_js_task_begin(runtime, OS64_JS_ABI_ID, "release", &outcome) == OS64_JS_OK &&
          task_call(runtime, "release", NULL, &outcome) == OS64_JS_OK &&
          os64_js_task_end(runtime, OS64_JS_ABI_ID, &closing) == OS64_JS_OK &&
          os64_js_module_state(runtime, waits, NULL) == OS64_JS_MODULE_FULFILLED, "and fulfilled in a later task");
    os64_js_module_t *later = module_compile(runtime, "https://m.test/later.js",
        "await new Promise((_, j) => globalThis.failLater = () => j(Error('later')));", &outcome);
    host_heard[0] = '\0';
    check(module_task(runtime, later, &outcome) == OS64_JS_OK &&
          os64_js_task_begin(runtime, OS64_JS_ABI_ID, "fail", &outcome) == OS64_JS_OK &&
          task_call(runtime, "failLater", NULL, &outcome) == OS64_JS_OK &&
          os64_js_task_end(runtime, OS64_JS_ABI_ID, &closing) == OS64_JS_OK &&
          os64_js_module_state(runtime, later, &failure) == OS64_JS_MODULE_REJECTED && strstr(failure.message, "later") != NULL,
          "a rejection after a turn is the module's failure");
    check(!strcmp(host_heard, "exception:Error: later") &&
          !os64_js_take_rejection(runtime, OS64_JS_ABI_ID, &promise, &reason, NULL),
          "heard as an exception, and not left as an unhandled rejection");

    // A module two graphs share, whose resolve fails partway in the first
    // (one import found, one not): marked unresolved again, so the second
    // graph resolves it afresh once the missing one is there, rather than
    // linking through the import it never found (patch 0012, found by WPT).
    // The shared module is acyclic; the cycles below are the cases where a
    // module whose own resolve SUCCEEDED points at the one that failed.
    module_late = NULL;
    os64_js_module_t *shared = module_compile(runtime, "https://m.test/shared.js", NULL, &outcome);
    os64_js_module_t *first = module_compile(runtime, "https://m.test/first.js", NULL, &outcome);
    os64_js_module_t *second = module_compile(runtime, "https://m.test/second.js", NULL, &outcome);
    check(module_task(runtime, first, &outcome) == OS64_JS_EXCEPTION && strstr(outcome.message, "late.js") != NULL,
          "a shared module's missing import fails the first graph's run");
    module_late = "export const late = 'late';";
    check(module_task(runtime, second, &outcome) == OS64_JS_OK &&
          os64_js_module_state(runtime, second, NULL) == OS64_JS_MODULE_FULFILLED,
          "the second graph resolves the shared module afresh and runs");
    JSValue late = task_global(runtime, "shared");
    check(host_value_is(late, "late"), "with the import that arrived");
    task_free(runtime, late);
    (void)shared;

    // A cycle whose entry fails after its member resolved back to it: the
    // member is not left resolved through a module the failure unlisted
    // (or freed), so a later graph reaching the member resolves it again
    // and fails cleanly, rather than linking through the dead entry. Each
    // module's handle held, as a page's map holds them.
    os64_js_module_t *cyc_e = module_compile(runtime, "https://m.test/cyc-e.js", NULL, &outcome);
    os64_js_module_t *cyc_a = module_compile(runtime, "https://m.test/cyc-a.js", NULL, &outcome);
    os64_js_module_t *cyc_g = module_compile(runtime, "https://m.test/cyc-g.js", NULL, &outcome);
    check(module_task(runtime, cyc_e, &outcome) == OS64_JS_EXCEPTION && strstr(outcome.message, "bare") != NULL,
          "a cycle's entry fails on its own import");
    check(module_task(runtime, cyc_g, &outcome) == OS64_JS_EXCEPTION && strstr(outcome.message, "bare") != NULL,
          "a later graph through the cycle's member fails the same way, linking nothing");
    (void)cyc_a;
    // The same cycle reached by import(), where the loader compiles both
    // and the failure frees the entry outright.
    os64_js_module_t *dyn = module_compile(runtime, "https://m.test/dyn-entry.js",
        "import('./dyn-x.js').catch(e => { globalThis.xerr = String(e); return import('./dyn-y.js'); })"
        ".catch(e => globalThis.yerr = String(e));", &outcome);
    check(module_task(runtime, dyn, &outcome) == OS64_JS_OK, "an entry importing a failing cycle runs");
    JSValue xerr = task_global(runtime, "xerr"), yerr = task_global(runtime, "yerr");
    check(JS_IsString(xerr) && JS_IsString(yerr), "both imports reject, the second through the freed entry's member");
    task_free(runtime, xerr); task_free(runtime, yerr);

    os64_js_module_release(runtime, b);
    os64_js_module_release(runtime, NULL);
    check(os64_js_set_script_host(runtime, OS64_JS_ABI_ID, NULL, &outcome) == OS64_JS_OK, "module script host cleared");
    (void)c2;
    os64_js_destroy(runtime);       /* the handles still held go with it */
    fixture_runtime = NULL;
    task_context = NULL;
}
/* A graph's every module compiled, uncut: a compile under memory pressure
 * is DEBTS' 0011b row (resolve_labels), which this sweep is not about, so
 * the dynamic import's target is compiled too and the run's loader never
 * compiles. */
static void module_scenario_compile(os64_js_runtime_t *runtime, os64_js_module_t **graph)
{
    os64_js_outcome_t outcome;
    const os64_js_module_loader_t loader = {(void *)module_store, module_resolve, module_source};
    os64_js_set_module_loader(runtime, OS64_JS_ABI_ID, &loader, &outcome);
    static const char *const names[] = {"https://m.test/a.js", "https://m.test/b.js", "https://m.test/lazy.js",
                                        "https://m.test/dyn.js", "https://m.test/entry.js"};
    const char *entry = "import lazy from './lazy.js'; import('./dyn.js').then(m => globalThis.dyn = m.default);";
    for (int i = 0; i < 5; i++) {
        const char *text = i == 4 ? entry : module_text(names[i]);
        os64_js_module_compile(runtime, OS64_JS_ABI_ID, text, strlen(text), names[i], NULL, &graph[i], &outcome);
    }
}
/* The rest of a graph's life, with whatever the runtime will still do after
 * a failure: its requests read, its entries run, its state asked, a handle
 * let go. */
static void module_scenario_run(os64_js_runtime_t *runtime, os64_js_module_t **graph)
{
    os64_js_outcome_t outcome, closing;
    os64_js_module_requests(runtime, OS64_JS_ABI_ID, graph[0], module_request, NULL, &outcome);
    if (os64_js_task_begin(runtime, OS64_JS_ABI_ID, "sweep", &outcome) == OS64_JS_OK) {
        os64_js_module_run(runtime, OS64_JS_ABI_ID, graph[0], &outcome);
        os64_js_module_run(runtime, OS64_JS_ABI_ID, graph[4], &outcome);
        os64_js_task_end(runtime, OS64_JS_ABI_ID, &closing);
    }
    os64_js_module_state(runtime, graph[4], NULL);
    os64_js_module_release(runtime, graph[1]);
}
/* Every allocation of a graph's run failed in turn, under the reclaim
 * policy a browser runs with: nothing crashes, and destroy reclaims all. */
static void module_sweep_cases(void)
{
    os64_js_config_t config = fixture_config();
    os64_js_outcome_t outcome;
    os64_js_runtime_t *runtime = NULL;
    os64_js_module_t *graph[5];
    os64_js_create_with_teardown(&config, OS64_JS_TEARDOWN_RECLAIM, OS64_JS_ABI_ID, &runtime, &outcome);
    if (!runtime) { check(false, "sweep runtime"); return; }
    module_scenario_compile(runtime, graph);
    size_t start = attempts;
    module_scenario_run(runtime, graph);
    size_t inventory = attempts - start;
    os64_js_destroy(runtime);
    check(live == 0 && inventory > 0, "the module scenario's allocations are counted");
    size_t failed = 0;
    for (size_t cut = 1; cut <= inventory; cut++) {
        runtime = NULL;
        os64_js_create_with_teardown(&config, OS64_JS_TEARDOWN_RECLAIM, OS64_JS_ABI_ID, &runtime, &outcome);
        if (!runtime) { check(false, "sweep runtime"); return; }
        module_scenario_compile(runtime, graph);
        fail_at = attempts + cut;
        module_scenario_run(runtime, graph);
        fail_at = 0;
        os64_js_teardown_report_t report;
        os64_js_destroy_report(runtime, &report);
        failed += live != 0;
    }
    check(failed == 0, "every failed allocation of a module graph is reclaimed");
    printf("libjs modules: %zu allocations swept\n", inventory);
}

/* BYTECODE_CACHE.md § 4: a classic script's blob, kept and run from. */
static const char bytecode_source[] =
    "var answer = (function outer(n){ function inner(k){ return k * 6 } return inner(n) })(7);";

static bool bytecode_answer_is(os64_js_runtime_t *runtime, int want)
{
    char probe[96];
    os64_js_outcome_t outcome;
    snprintf(probe, sizeof(probe), "if (answer !== %d) throw new Error('answer is ' + answer)", want);
    return os64_js_eval(runtime, probe, strlen(probe), "probe.js", &outcome) == OS64_JS_OK;
}

/* A blob of `source` compiled under `name`, the caller's to free. */
static uint8_t *bytecode_blob(const char *source, const char *name, size_t *length)
{
    os64_js_config_t config = fixture_config();
    os64_js_runtime_t *runtime = create_fixture(&config);
    if (runtime == NULL) return NULL;
    os64_js_outcome_t outcome;
    os64_js_bytecode_t bc = {.keep = true};
    os64_js_run_bytecode(runtime, source, strlen(source), name, &bc, &outcome);
    os64_js_destroy(runtime);
    *length = bc.kept_length;
    return bc.kept;
}

/* A script of a few hundred names and functions whose answer is 42, with no
 * jump in it: a jump is compiled through a label, and a compile's failed
 * allocation in resolve_labels still crashes (DEBTS.md), which is the
 * parser's debt and not what these sweeps are after. */
static char *bytecode_large_source(void)
{
    size_t cap = 64 * 1024, at = 0;
    char *text = malloc(cap);
    if (text == NULL) return NULL;
    at += (size_t)snprintf(text + at, cap - at, "var answer = (function(){ var o = {");
    for (int i = 0; i < 300; i++) at += (size_t)snprintf(text + at, cap - at, "name%d: %d, ", i, i);
    at += (size_t)snprintf(text + at, cap - at, "}; var total = 0;");
    for (int i = 0; i < 60; i++)
        at += (size_t)snprintf(text + at, cap - at,
                               "function f%d(k){ var s%d = 'text %d'; return k + s%d.length - %zu }"
                               " total += f%d(o.name%d);",
                               i, i, i, i, strlen("text ") + (size_t)snprintf(NULL, 0, "%d", i), i, i);
    snprintf(text + at, cap - at, " return 42 + total - %d })();", 59 * 60 / 2);
    return text;
}

/* Each allocation of a keeping run of `source` failed in turn: one the
 * write makes is taken back and the script's run goes on, keeping nothing;
 * any other fails the runtime as it always has, with the blob kept when the
 * compile and the write were done before it (the blob is the source's,
 * whatever became of its run). Every blob kept reads back and runs to the
 * script's answer, and nothing leaks either way. */
static void bytecode_keep_sweep(const char *name, const char *source)
{
    os64_js_config_t config = fixture_config();
    os64_js_outcome_t outcome;
    os64_js_runtime_t *runtime = create_fixture(&config);
    size_t start = attempts;
    os64_js_bytecode_t bc = {.keep = true};
    bool whole = os64_js_run_bytecode(runtime, source, strlen(source), "sweep.js", &bc, &outcome) == OS64_JS_OK;
    size_t inventory = attempts - start;
    check(whole && bc.kept != NULL && bytecode_answer_is(runtime, 42), "a sweep's script runs whole and keeps its blob");
    os64_free(bc.kept);
    os64_js_destroy(runtime);
    // What the write alone allocates: the same run, keeping nothing.
    runtime = create_fixture(&config);
    start = attempts;
    bc = (os64_js_bytecode_t){0};
    os64_js_run_bytecode(runtime, source, strlen(source), "sweep.js", &bc, &outcome);
    size_t writes = inventory - (attempts - start);
    os64_js_destroy(runtime);
    size_t held = live, absorbed = 0, kept = 0, wrong = 0;
    for (size_t cut = 1; cut <= inventory; cut++) {
        runtime = create_fixture(&config);
        bc = (os64_js_bytecode_t){.keep = true};
        fail_at = attempts + cut;
        os64_js_status_t result = os64_js_run_bytecode(runtime, source, strlen(source), "sweep.js", &bc, &outcome);
        fail_at = 0;
        bool bad = false;
        if (result == OS64_JS_OK && bc.kept == NULL) {
            absorbed++;
            bad = !bytecode_answer_is(runtime, 42);
        } else if (result != OS64_JS_OK && result != OS64_JS_HOST_FAILURE) {
            bad = true;
        }
        os64_js_destroy(runtime);
        if (bc.kept != NULL) {
            kept++;
            runtime = create_fixture(&config);
            os64_js_bytecode_t again = {.blob = bc.kept, .blob_length = bc.kept_length};
            bad |= os64_js_run_bytecode(runtime, source, strlen(source), "again.js", &again, &outcome) != OS64_JS_OK ||
                   !again.from_blob || !bytecode_answer_is(runtime, 42);
            os64_js_destroy(runtime);
        }
        bool was_kept = bc.kept != NULL;
        os64_free(bc.kept);
        bad |= live != held;
        if (bad)
            fprintf(stderr, "libjs bytecode: %s keeping cut %zu answered %d, %s, %zu live\n", name, cut,
                    (int)result, was_kept ? "kept" : "not kept", live - held);
        wrong += bad;
    }
    printf("libjs bytecode: %s: keeping %zu allocations swept, %zu absorbed by the write of %zu, %zu blobs read back\n",
           name, inventory, absorbed, writes, kept);
    // Every allocation the write makes is the write's to absorb: a refusal
    // it did not take back would fail the runtime instead.
    check(absorbed > 0 && absorbed == writes && wrong == 0,
          "a keeping run's failed allocation is the write's to absorb or the runtime's, every blob kept "
          "reads back whole, and nothing leaks");
}

/* Each allocation of a run from `source`'s blob failed in turn: a failed
 * read is the runtime's failure, as a compile's would be, whatever the
 * reader answered, and nothing leaks. */
static void bytecode_read_sweep(const char *name, const char *source)
{
    size_t length = 0;
    uint8_t *blob = bytecode_blob(source, "kept.js", &length);
    if (blob == NULL) { check(false, "a read sweep's blob"); return; }
    os64_js_config_t config = fixture_config();
    os64_js_outcome_t outcome;
    os64_js_runtime_t *runtime = create_fixture(&config);
    size_t start = attempts;
    os64_js_bytecode_t bc = {.blob = blob, .blob_length = length};
    bool whole = os64_js_run_bytecode(runtime, source, strlen(source), "read.js", &bc, &outcome) == OS64_JS_OK;
    size_t inventory = attempts - start;
    check(whole && bc.from_blob && bytecode_answer_is(runtime, 42), "a read sweep's script runs whole from its blob");
    os64_js_destroy(runtime);
    size_t held = live, wrong = 0;
    for (size_t cut = 1; cut <= inventory; cut++) {
        runtime = create_fixture(&config);
        bc = (os64_js_bytecode_t){.blob = blob, .blob_length = length};
        fail_at = attempts + cut;
        os64_js_status_t result = os64_js_run_bytecode(runtime, source, strlen(source), "read.js", &bc, &outcome);
        fail_at = 0;
        bool bad = result != OS64_JS_HOST_FAILURE;
        os64_js_destroy(runtime);
        bad |= live != held;
        if (bad)
            fprintf(stderr, "libjs bytecode: %s reading cut %zu answered %d, %zu live\n", name, cut, (int)result,
                    live - held);
        wrong += bad;
    }
    printf("libjs bytecode: %s: reading %zu allocations swept\n", name, inventory);
    check(wrong == 0, "a failed allocation reading a blob fails the runtime and leaks nothing");
    os64_free(blob);
}

static void bytecode_cases(void)
{
    os64_js_config_t config = fixture_config();
    os64_js_outcome_t outcome;
    size_t length = 0;
    uint8_t *blob = bytecode_blob(bytecode_source, "first.js", &length);
    check(blob != NULL && length > 96, "a compile keeps its blob when asked");

    // The blob runs in another runtime, and what it ran is the script.
    os64_js_runtime_t *runtime = create_fixture(&config);
    os64_js_bytecode_t bc = {.blob = blob, .blob_length = length, .keep = true};
    check(os64_js_run_bytecode(runtime, bytecode_source, strlen(bytecode_source), "second.js", &bc,
                               &outcome) == OS64_JS_OK &&
          bc.from_blob && bc.kept == NULL && bytecode_answer_is(runtime, 42),
          "a blob runs as its script, and keeps nothing, there being no compile");
    os64_js_destroy(runtime);

    // The name a thrown error carries is the run's, never the compile's,
    // at every depth of the script's functions.
    const char *thrower = "function deep(){ throw new Error('boom') } (function(){ deep() })();";
    size_t thrown_length = 0;
    uint8_t *thrown = bytecode_blob(thrower, "os64-script:3", &thrown_length);
    runtime = create_fixture(&config);
    bc = (os64_js_bytecode_t){.blob = thrown, .blob_length = thrown_length};
    check(os64_js_run_bytecode(runtime, thrower, strlen(thrower), "os64-script:7", &bc, &outcome) ==
              OS64_JS_EXCEPTION && bc.from_blob &&
          strstr(outcome.stack_trace, "os64-script:7") != NULL &&
          strstr(outcome.stack_trace, "os64-script:3") == NULL,
          "a blob's functions throw under the name of the run that read it");
    os64_js_destroy(runtime);
    os64_free(thrown);

    // Every refusal is a miss: the source runs, and a fresh blob is kept.
    static const struct { const char *what; size_t at; uint8_t value; size_t cut; } refused[] = {
        {"its magic", 0, 'X', 0},
        {"another engine's identity", 12, 'z', 0},
        {"another kind", 76, 'm', 0},
        {"a payload byte", SIZE_MAX, 0x5a, 0},
        {"a length one short", 0, 0, 1},
        {"a head cut short", 0, 0, SIZE_MAX},
    };
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        uint8_t *copy = malloc(length);
        memcpy(copy, blob, length);
        size_t copy_length = length;
        if (refused[i].cut == SIZE_MAX) copy_length = 40;
        else if (refused[i].cut != 0) copy_length -= refused[i].cut;
        else {
            size_t at = refused[i].at == SIZE_MAX ? length - 1 : refused[i].at;
            copy[at] ^= refused[i].value;
        }
        runtime = create_fixture(&config);
        bc = (os64_js_bytecode_t){.blob = copy, .blob_length = copy_length, .keep = true};
        const char *other = "var answer = 7;";
        os64_js_status_t result = os64_js_run_bytecode(runtime, other, strlen(other), "refused.js", &bc, &outcome);
        char name[96];
        snprintf(name, sizeof(name), "a blob with %s is refused, its source run and kept", refused[i].what);
        check(result == OS64_JS_OK && !bc.from_blob && bc.kept != NULL && bytecode_answer_is(runtime, 7), name);
        os64_free(bc.kept);
        os64_js_destroy(runtime);
        free(copy);
    }

    // A compile that throws keeps nothing; a missing bytecode is refused.
    runtime = create_fixture(&config);
    bc = (os64_js_bytecode_t){.keep = true};
    check(os64_js_run_bytecode(runtime, "var = ;", 7, "syntax.js", &bc, &outcome) == OS64_JS_EXCEPTION &&
          bc.kept == NULL && !bc.from_blob && strcmp(outcome.message, "SyntaxError: variable name expected") == 0,
          "a compile that throws keeps no blob, and reports its own error");
    check(os64_js_run_bytecode(runtime, "1", 1, "none.js", NULL, &outcome) == OS64_JS_BAD_ARGUMENT,
          "running through no bytecode is refused");
    os64_js_destroy(runtime);

    bytecode_keep_sweep("small", bytecode_source);
    // Large enough that the writer's buffer and atom table grow as it
    // writes, which is where upstream's writer ignored a failure and handed
    // back a payload that read as a broken function.
    char *large = bytecode_large_source();
    if (large != NULL) bytecode_keep_sweep("large", large);
    bytecode_read_sweep("small", bytecode_source);
    if (large != NULL) bytecode_read_sweep("large", large);
    free(large);
    os64_free(blob);
}

/* BYTECODE_CACHE.md § 9: a module's blob, written under the module's name
 * and read only by a compile of that name. */
static os64_js_module_t *module_compile_through(os64_js_runtime_t *runtime, const char *name,
                                                os64_js_bytecode_t *bc, os64_js_outcome_t *outcome)
{
    const char *source = module_text(name);
    os64_js_module_t *module = NULL;
    os64_js_status_t result = os64_js_module_compile_bytecode(runtime, OS64_JS_ABI_ID, source, strlen(source), name,
                                                              NULL, bc, &module, outcome);
    check(result == outcome->status, "module bytecode compile status matches outcome");
    return module;
}
/* A fresh module runtime: the store's loader, and an empty order. */
static os64_js_runtime_t *module_bytecode_fixture(void)
{
    os64_js_config_t config = fixture_config();
    os64_js_outcome_t outcome;
    os64_js_runtime_t *runtime = task_fixture(&config, "globalThis.order = '';");
    const os64_js_module_loader_t loader = {(void *)module_store, module_resolve, module_source};
    if (runtime != NULL) os64_js_set_module_loader(runtime, OS64_JS_ABI_ID, &loader, &outcome);
    return runtime;
}
static void module_bytecode_fixture_end(os64_js_runtime_t *runtime)
{
    os64_js_destroy(runtime);
    fixture_runtime = NULL;
    task_context = NULL;
}
static bool module_order_is(os64_js_runtime_t *runtime, const char *want)
{
    JSValue order = task_global(runtime, "order");
    bool is = host_value_is(order, want);
    task_free(runtime, order);
    return is;
}
/* A module compiled through `bytecode`, compile only: its handle is the
 * runtime's to let go at destroy. */
static os64_js_status_t module_bytecode_compile(os64_js_runtime_t *runtime, const char *source,
                                                os64_js_bytecode_t *bc, os64_js_outcome_t *outcome)
{
    os64_js_module_t *module = NULL;
    return os64_js_module_compile_bytecode(runtime, OS64_JS_ABI_ID, source, strlen(source), "https://m.test/sweep.js",
                                           NULL, bc, &module, outcome);
}
/* bytecode_keep_sweep's rules for a module's compile: a failed allocation
 * of the write is taken back and the compile stands, keeping nothing; any
 * other fails the runtime; every blob kept reads back as the module. A
 * module's compile under a failed allocation is DEBTS' 0011b row
 * (resolve_labels), so the sweep starts at the last allocation and stops at
 * the write's first: every cut it makes falls after the compile. */
static void module_bytecode_keep_sweep(const char *name, const char *source)
{
    os64_js_config_t config = fixture_config();
    os64_js_outcome_t outcome;
    os64_js_runtime_t *runtime = create_fixture(&config);
    size_t start = attempts;
    os64_js_bytecode_t bc = {.keep = true};
    bool whole = module_bytecode_compile(runtime, source, &bc, &outcome) == OS64_JS_OK;
    size_t inventory = attempts - start;
    check(whole && bc.kept != NULL, "a module sweep's compile is whole and keeps its blob");
    os64_free(bc.kept);
    os64_js_destroy(runtime);
    runtime = create_fixture(&config);
    start = attempts;
    bc = (os64_js_bytecode_t){0};
    module_bytecode_compile(runtime, source, &bc, &outcome);
    size_t writes = inventory - (attempts - start);
    os64_js_destroy(runtime);
    size_t held = live, absorbed = 0, kept = 0, wrong = 0, swept = 0;
    for (size_t cut = inventory; cut > 0 && absorbed < writes; cut--) {
        swept++;
        runtime = create_fixture(&config);
        bc = (os64_js_bytecode_t){.keep = true};
        fail_at = attempts + cut;
        os64_js_status_t result = module_bytecode_compile(runtime, source, &bc, &outcome);
        fail_at = 0;
        bool bad = result != OS64_JS_OK && result != OS64_JS_HOST_FAILURE;
        if (result == OS64_JS_OK && bc.kept == NULL) absorbed++;
        os64_js_destroy(runtime);
        if (bc.kept != NULL) {
            kept++;
            runtime = create_fixture(&config);
            os64_js_bytecode_t again = {.blob = bc.kept, .blob_length = bc.kept_length};
            bad |= module_bytecode_compile(runtime, source, &again, &outcome) != OS64_JS_OK || !again.from_blob;
            os64_js_destroy(runtime);
        }
        os64_free(bc.kept);
        bad |= live != held;
        if (bad)
            fprintf(stderr, "libjs module bytecode: %s keeping cut %zu answered %d, %zu live\n", name, cut,
                    (int)result, live - held);
        wrong += bad;
    }
    printf("libjs module bytecode: %s: keeping the last %zu of %zu allocations swept, %zu absorbed by the write "
           "of %zu, %zu blobs read back\n", name, swept, inventory, absorbed, writes, kept);
    check(absorbed > 0 && absorbed == writes && wrong == 0,
          "a keeping module compile's failed allocation is the write's to absorb or the runtime's, every blob "
          "kept reads back, and nothing leaks");
}
/* bytecode_read_sweep's rule for a module's: a failed allocation reading
 * the blob fails the runtime, and nothing leaks. */
static void module_bytecode_read_sweep(const char *name, const char *source)
{
    os64_js_config_t config = fixture_config();
    os64_js_outcome_t outcome;
    os64_js_runtime_t *runtime = create_fixture(&config);
    os64_js_bytecode_t bc = {.keep = true};
    module_bytecode_compile(runtime, source, &bc, &outcome);
    os64_js_destroy(runtime);
    uint8_t *blob = bc.kept;
    size_t length = bc.kept_length;
    if (blob == NULL) { check(false, "a module read sweep's blob"); return; }
    runtime = create_fixture(&config);
    size_t start = attempts;
    bc = (os64_js_bytecode_t){.blob = blob, .blob_length = length};
    bool whole = module_bytecode_compile(runtime, source, &bc, &outcome) == OS64_JS_OK;
    size_t inventory = attempts - start;
    check(whole && bc.from_blob, "a module read sweep's compile is whole from its blob");
    os64_js_destroy(runtime);
    size_t held = live, wrong = 0;
    for (size_t cut = 1; cut <= inventory; cut++) {
        runtime = create_fixture(&config);
        bc = (os64_js_bytecode_t){.blob = blob, .blob_length = length};
        fail_at = attempts + cut;
        os64_js_status_t result = module_bytecode_compile(runtime, source, &bc, &outcome);
        fail_at = 0;
        bool bad = result != OS64_JS_HOST_FAILURE;
        os64_js_destroy(runtime);
        bad |= live != held;
        if (bad)
            fprintf(stderr, "libjs module bytecode: %s reading cut %zu answered %d, %zu live\n", name, cut,
                    (int)result, live - held);
        wrong += bad;
    }
    printf("libjs module bytecode: %s: reading %zu allocations swept\n", name, inventory);
    check(wrong == 0, "a failed allocation reading a module's blob fails the runtime and leaks nothing");
    os64_free(blob);
}
static void module_bytecode_cases(void)
{
    os64_js_outcome_t outcome;
    const char *a_name = "https://m.test/a.js", *b_name = "https://m.test/b.js";

    // A graph compiled keeping each module's blob, and run.
    os64_js_runtime_t *runtime = module_bytecode_fixture();
    if (runtime == NULL) return;
    os64_js_bytecode_t kept_a = {.keep = true}, kept_b = {.keep = true};
    os64_js_module_t *a = module_compile_through(runtime, a_name, &kept_a, &outcome);
    os64_js_module_t *b = module_compile_through(runtime, b_name, &kept_b, &outcome);
    check(a != NULL && b != NULL && kept_a.kept != NULL && kept_b.kept != NULL && !kept_a.from_blob &&
          !kept_b.from_blob, "a module's compile keeps its blob when asked");
    check(module_task(runtime, a, &outcome) == OS64_JS_OK && module_order_is(runtime, "ba"),
          "the graph whose blobs were kept runs");
    module_bytecode_fixture_end(runtime);

    // The blobs, in another runtime, are the graph: its requests named, its
    // imports found by name, import.meta.url the compile's own.
    runtime = module_bytecode_fixture();
    if (runtime == NULL) return;
    os64_js_bytecode_t read_a = {.blob = kept_a.kept, .blob_length = kept_a.kept_length, .keep = true};
    os64_js_bytecode_t read_b = {.blob = kept_b.kept, .blob_length = kept_b.kept_length, .keep = true};
    a = module_compile_through(runtime, a_name, &read_a, &outcome);
    module_requests[0] = '\0';
    check(a != NULL && read_a.from_blob && read_a.kept == NULL &&
          os64_js_module_requests(runtime, OS64_JS_ABI_ID, a, module_request, NULL, &outcome) == OS64_JS_OK &&
          !strcmp(module_requests, "./b.js"), "a module read from its blob names its requests, and keeps nothing");
    b = module_compile_through(runtime, b_name, &read_b, &outcome);
    module_loads = 0;
    check(b != NULL && read_b.from_blob && module_task(runtime, a, &outcome) == OS64_JS_OK &&
          os64_js_module_state(runtime, a, NULL) == OS64_JS_MODULE_FULFILLED && module_order_is(runtime, "ba") &&
          module_loads == 0, "a graph read from its blobs runs as compiled, each import found by name");
    const char *meta_source = "globalThis.meta = import.meta.url;";
    os64_js_bytecode_t meta_kept = {.keep = true};
    os64_js_module_t *meta = NULL;
    os64_js_module_compile_bytecode(runtime, OS64_JS_ABI_ID, meta_source, strlen(meta_source),
                                    "https://m.test/page#inline-1", "https://m.test/page", &meta_kept, &meta, &outcome);
    module_bytecode_fixture_end(runtime);
    runtime = module_bytecode_fixture();
    if (runtime == NULL) return;
    os64_js_bytecode_t meta_read = {.blob = meta_kept.kept, .blob_length = meta_kept.kept_length};
    check(os64_js_module_compile_bytecode(runtime, OS64_JS_ABI_ID, meta_source, strlen(meta_source),
                                          "https://m.test/page#inline-1", "https://m.test/other", &meta_read, &meta,
                                          &outcome) == OS64_JS_OK && meta_read.from_blob &&
          module_task(runtime, meta, &outcome) == OS64_JS_OK, "an inline module runs from its blob");
    JSValue url = task_global(runtime, "meta");
    check(host_value_is(url, "https://m.test/other"), "import.meta.url is the reading compile's, not the blob's");
    task_free(runtime, url);
    os64_free(meta_kept.kept);
    module_bytecode_fixture_end(runtime);

    // A blob written for another module, or for a classic script, or a
    // module's offered to a classic run, is a miss: the source is compiled.
    runtime = module_bytecode_fixture();
    if (runtime == NULL) return;
    os64_js_bytecode_t other = {.blob = kept_a.kept, .blob_length = kept_a.kept_length, .keep = true};
    b = module_compile_through(runtime, b_name, &other, &outcome);
    check(b != NULL && !other.from_blob && other.kept != NULL, "a blob written under another name is refused");
    os64_free(other.kept);
    size_t classic_length = 0;
    uint8_t *classic = bytecode_blob(bytecode_source, "https://m.test/a.js", &classic_length);
    os64_js_bytecode_t script = {.blob = classic, .blob_length = classic_length};
    a = module_compile_through(runtime, a_name, &script, &outcome);
    check(a != NULL && !script.from_blob, "a classic script's blob is refused by a module compile");
    check(module_task(runtime, a, &outcome) == OS64_JS_OK && module_order_is(runtime, "ba"),
          "the modules compiled after refusals run");
    os64_js_bytecode_t as_script = {.blob = kept_b.kept, .blob_length = kept_b.kept_length};
    check(os64_js_run_bytecode(runtime, bytecode_source, strlen(bytecode_source), "https://m.test/b.js", &as_script,
                               &outcome) == OS64_JS_OK && !as_script.from_blob && bytecode_answer_is(runtime, 42),
          "a module's blob is refused by a classic run");
    os64_free(classic);

    // A compile that throws keeps nothing; no bytecode is refused.
    os64_js_bytecode_t bad = {.keep = true};
    check(module_compile_through(runtime, "https://m.test/bad.js", &bad, &outcome) == NULL &&
          outcome.status == OS64_JS_EXCEPTION && bad.kept == NULL && !bad.from_blob,
          "a module compile that throws keeps no blob");
    os64_js_module_t *none = NULL;
    check(os64_js_module_compile_bytecode(runtime, OS64_JS_ABI_ID, "", 0, "https://m.test/none.js", NULL, NULL, &none,
                                          &outcome) == OS64_JS_BAD_ARGUMENT && none == NULL,
          "a module compile through no bytecode is refused");
    module_bytecode_fixture_end(runtime);
    os64_free(kept_a.kept);
    os64_free(kept_b.kept);

    const char *small = module_text("https://m.test/a.js");
    module_bytecode_keep_sweep("small", small);
    module_bytecode_read_sweep("small", small);
    char *large = bytecode_large_source();
    if (large != NULL) {
        module_bytecode_keep_sweep("large", large);
        module_bytecode_read_sweep("large", large);
    }
    free(large);
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
    tracked_allocator_cases(); tracked_construction_cases(); set_prototype_refused_cases(); teardown_cases(); runtime_cases(); clock_cases(); diagnostic_cases(); regexp_unicode_sets_cases(); allocation_cases(); thread_cases();
    file_cases(); file_boundary_cases(); output_cases(); console_cases(); transaction_descriptor_cases(); transaction_trap_case(); setup_allocation_cases();
    task_cases();
    script_host_cases();
    module_cases();
    module_sweep_cases();
    bytecode_cases();
    module_bytecode_cases();
    check(live == 0, "runtime suite releases every fixture allocation");
    printf("libjs runtime: %u checks, %u failures, %zu live allocations, %u native calls\n",
           checks, failures, live, native_calls);
    return failures != 0;
}
