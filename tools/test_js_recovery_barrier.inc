/* These raw-engine tests use terminal diagnostic constructors. */
static JSRuntime *barrier_engine;
static JSContext *barrier_context, *barrier_child;
static JSRecoveryBarrier barrier_token;
static JSValue barrier_function;
static unsigned barrier_effects, barrier_probes, barrier_loader_hits, barrier_jobs;
static JSClassID barrier_exotic_class_id, barrier_cleanup_class_id;
static bool barrier_exotic_throws, barrier_expect_resolve_scope;
static int barrier_job_order[4];

static void barrier_probe(JSRuntime *rt)
{
    JSExecutionState state;
    check(JS_GetExecutionState(rt, &state) && state.tracking &&
          (state.scopes || state.interpreter || state.native_callbacks || state.settings || state.gc),
          "native hook reports engine activity even without an interpreter frame");
    JSRecoveryBarrier unchanged = {NULL, 93}, refused = unchanged;
    size_t before = attempts;
    check(!JS_BeginRecoveryBarrier(rt, &refused) && refused.runtime == unchanged.runtime &&
          refused.generation == unchanged.generation,
          "native hook cannot begin a recovery fence or modify refused token");
    check(!JS_MarkContextExecutableFailed(barrier_child), "native hook cannot mark executable failure beneath active engine work");
    if (state.blocked) {
        check(!JS_EndRecoveryBarrier(rt, &barrier_token), "cleanup callback cannot release its enclosing recovery fence");
        JSValue result = JS_Call(barrier_context, barrier_function, JS_UNDEFINED, 0, NULL);
        check(JS_IsException(result), "cleanup callback cannot reenter healthy code while fenced");
        JS_FreeValue(barrier_context, result);
    }
    check(attempts == before, "native fence refusal and blocked reentry attempt no allocation");
    barrier_probes++;
}
static JSValue barrier_hit(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)ctx; (void)self; (void)argc; (void)argv;
    barrier_effects++;
    return JS_UNDEFINED;
}
static JSValue barrier_native_probe(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self; (void)argc; (void)argv;
    barrier_probe(JS_GetRuntime(ctx));
    return JS_UNDEFINED;
}
static JSValue barrier_data_probe(JSContext *ctx, JSValueConst self, int argc,
                                  JSValueConst *argv, int magic, JSValue *data)
{
    (void)magic; (void)data;
    return barrier_native_probe(ctx, self, argc, argv);
}
static JSValue barrier_iterator(JSContext *ctx, JSValueConst self, int argc,
                                JSValueConst *argv, int *done, int magic)
{
    (void)magic;
    barrier_native_probe(ctx, self, argc, argv);
    *done = true;
    return JS_UNDEFINED;
}
static JSValue barrier_settings(JSContext *ctx, void *opaque)
{
    (void)opaque;
    barrier_probe(JS_GetRuntime(ctx));
    return JS_UNDEFINED;
}
static JSValue barrier_job(JSContext *ctx, int argc, JSValueConst *argv)
{
    barrier_probe(JS_GetRuntime(ctx));
    if (barrier_jobs < sizeof(barrier_job_order)/sizeof(barrier_job_order[0]))
        barrier_job_order[barrier_jobs] = argc ? JS_VALUE_GET_INT(argv[0]) : 0;
    barrier_jobs++;
    return JS_UNDEFINED;
}
static int barrier_module_init(JSContext *ctx, JSModuleDef *module)
{
    (void)module;
    barrier_probe(JS_GetRuntime(ctx));
    return 0;
}
static JSModuleDef *barrier_module_loader(JSContext *ctx, const char *name, void *opaque)
{
    (void)opaque;
    barrier_probe(JS_GetRuntime(ctx)); barrier_loader_hits++;
    if (barrier_expect_resolve_scope) {
        execution_take(JS_GetRuntime(ctx));
        check(execution_has(JS_EXECUTION_SCOPE_MODULE_RESOLVE, ctx),
              "direct module resolution exposes its native-loader execution scope");
    }
    return JS_NewCModule(ctx, name, barrier_module_init);
}
static void barrier_finalizer(JSRuntime *rt, JSValue value)
{
    (void)value;
    barrier_probe(rt);
    JSExecutionState state;
    if (JS_GetExecutionState(rt, &state) && state.blocked) {
        JSValue tag = JS_NewInt32(barrier_context, 1);
        check(JS_EnqueueJob(barrier_context, barrier_job, 1, &tag) == 0,
              "native cleanup can enqueue work without executing it beneath the fence");
    }
}
static void barrier_gc_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    (void)value; (void)mark;
    JSExecutionState state;
    check(JS_GetExecutionState(rt, &state) && state.gc, "GC marker query reports its active collection phase");
    barrier_probe(rt);
}
static void barrier_mark_function(JSRuntime *rt, JSGCObjectHeader *header)
{
    (void)header;
    barrier_probe(rt);
}
static void barrier_buffer_free(JSRuntime *rt, void *opaque, void *data)
{
    (void)opaque;
    barrier_probe(rt); os64_free(data);
}
static void barrier_print(void *opaque, const char *bytes, size_t length)
{
    (void)bytes; (void)length;
    barrier_probe(opaque);
}
static void barrier_global_miss(void *opaque, const char *name)
{
    (void)name;
    barrier_probe(opaque);
}
static int barrier_interrupt(JSRuntime *rt, void *opaque)
{
    (void)opaque;
    barrier_probe(rt);
    return 1;
}
static void barrier_rejection(JSContext *ctx, JSValueConst promise, JSValueConst reason,
                               JS_BOOL handled, void *opaque)
{
    (void)promise; (void)reason; (void)handled; (void)opaque;
    barrier_probe(JS_GetRuntime(ctx));
}
static JSValue barrier_exotic_get(JSContext *ctx, JSValueConst obj, JSAtom name,
                                  JSValueConst receiver)
{
    (void)obj; (void)name; (void)receiver;
    barrier_probe(JS_GetRuntime(ctx));
    return barrier_exotic_throws ? JS_EXCEPTION : JS_NewInt32(ctx, 17);
}
static int barrier_exotic_set(JSContext *ctx, JSValueConst obj, JSAtom name,
                              JSValueConst value, JSValueConst receiver, int flags)
{
    (void)obj; (void)name; (void)value; (void)receiver; (void)flags;
    barrier_probe(JS_GetRuntime(ctx));
    return 1;
}
static int barrier_exotic_has(JSContext *ctx, JSValueConst obj, JSAtom name)
{
    (void)obj; (void)name;
    barrier_probe(JS_GetRuntime(ctx));
    return 0;
}
static int barrier_exotic_delete(JSContext *ctx, JSValueConst obj, JSAtom name)
{
    return barrier_exotic_has(ctx, obj, name);
}
static int barrier_exotic_names(JSContext *ctx, JSPropertyEnum **names, uint32_t *count, JSValueConst obj)
{
    (void)obj;
    barrier_probe(JS_GetRuntime(ctx)); *names = NULL; *count = 0;
    return 0;
}
static JSValue barrier_exotic_proto(JSContext *ctx, JSValueConst obj)
{
    (void)obj; barrier_probe(JS_GetRuntime(ctx)); return JS_NULL;
}
static int barrier_exotic_set_proto(JSContext *ctx, JSValueConst obj, JSValueConst proto)
{
    (void)obj; (void)proto; barrier_probe(JS_GetRuntime(ctx)); return 1;
}
static int barrier_exotic_extensible(JSContext *ctx, JSValueConst obj)
{
    (void)obj; barrier_probe(JS_GetRuntime(ctx)); return 1;
}
static int barrier_exotic_prevent(JSContext *ctx, JSValueConst obj)
{
    return barrier_exotic_extensible(ctx, obj);
}
static int barrier_exotic_define(JSContext *ctx, JSValueConst obj, JSAtom name,
                                 JSValueConst value, JSValueConst getter, JSValueConst setter, int flags)
{
    (void)obj; (void)name; (void)value; (void)getter; (void)setter; (void)flags;
    barrier_probe(JS_GetRuntime(ctx)); return 1;
}
static int barrier_exotic_own(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst obj, JSAtom name)
{
    (void)desc; (void)obj; (void)name; barrier_probe(JS_GetRuntime(ctx)); return 0;
}
static void barrier_refused(JSContext *ctx, JSValue result)
{
    check(JS_IsException(result), "fenced executable entry returns refusal before body side effects");
    JS_FreeValue(ctx, result);
}
static void recovery_barrier_cases(void)
{
    size_t initial = live;
    JSExecutionState state;
    memset(&state, 0x5a, sizeof(state));
    check(!JS_GetExecutionState(NULL, &state) && !state.tracking && !state.blocked,
          "execution state query clears output for null runtime");
    check(!JS_GetExecutionState(NULL, NULL) && !JS_BeginRecoveryBarrier(NULL, NULL) &&
          !JS_EndRecoveryBarrier(NULL, NULL), "null execution barrier arguments are refused");
    JSRuntime *disabled = JS_NewRuntime();
    JSRecoveryBarrier untouched = {NULL, 77};
    check(disabled && !JS_BeginRecoveryBarrier(disabled, &untouched) && untouched.generation == 77,
          "raw fence requires explicit tracking and preserves refused token");
    JS_FreeRuntime(disabled);
    for (unsigned policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; policy++) {
        os64_js_config_t config = fixture_config();
        os64_js_runtime_t *runtime = NULL;
        os64_js_outcome_t out;
        check(os64_js_create_with_failure_records(&config, policy, OS64_JS_ABI_ID, &runtime, &out) == OS64_JS_OK,
              "barrier fixture constructed with opt-in tracking");
        if (!runtime) continue;
        os64_js_realm_t *child = NULL;
        check(os64_js_realm_create(runtime, OS64_JS_ABI_ID, &child, &out) == OS64_JS_OK,
              "barrier fixture child created");
        barrier_context = os64_js_context(runtime, OS64_JS_ABI_ID, &out);
        barrier_child = os64_js_realm_context(child, OS64_JS_ABI_ID, &out);
        barrier_engine = JS_GetRuntime(barrier_context);
        JSContext *a = barrier_context, *b = barrier_child;
        barrier_effects = barrier_probes = barrier_loader_hits = barrier_jobs = 0;
        barrier_exotic_throws = barrier_expect_resolve_scope = false;
        memset(barrier_job_order, -1, sizeof(barrier_job_order));
        barrier_function = JS_NewCFunction(a, barrier_hit, "hit", 0);
        JSValue global = JS_GetGlobalObject(a);
        JS_SetPropertyStr(a, global, "hit", JS_DupValue(a, barrier_function));
        JS_SetPropertyStr(a, global, "probe", JS_NewCFunction(a, barrier_native_probe, "probe", 0));
        JS_SetPropertyStr(a, global, "dataProbe", JS_NewCFunctionData(a, barrier_data_probe, 0, 0, 0, NULL));
        JSCFunctionType cb = {.iterator_next = barrier_iterator};
        JS_SetPropertyStr(a, global, "iteratorNext", JS_NewCFunction2(a, cb.generic, "iteratorNext", 0, JS_CFUNC_iterator_next, 0));
        JS_FreeValue(a, global);
        execution_eval(a, "probe();dataProbe();for(const x of {next:iteratorNext,[Symbol.iterator](){return this}}){};");
        JSValue value = JS_UNDEFINED;
        check(JS_RunWithSettings(a, NULL, barrier_settings, NULL, &value), "idle native settings fixture runs");
        JS_FreeValue(a, value);
        JS_SetInterruptHandler(barrier_engine, barrier_interrupt, NULL);
        value = JS_Eval(a, "while(true){}", 13, "barrier-interrupt.js", JS_EVAL_TYPE_GLOBAL);
        check(JS_IsException(value), "pre-entry/interrupt callback fixture unwinds");
        if (JS_IsException(value)) JS_FreeValue(a, JS_GetException(a));
        JS_FreeValue(a, value); JS_SetInterruptHandler(barrier_engine, NULL, NULL);
        JS_SetHostPromiseRejectionTracker(barrier_engine, barrier_rejection, NULL);
        execution_eval(a, "Promise.reject(7)");
        JS_SetHostPromiseRejectionTracker(barrier_engine, NULL, NULL);
        JSValue healthy = record_function(a, "(function(){hit()})");
        JSValue foreign = record_function(b, "(function(){return 42})");
        const char *compiled_source = "hit()";
        JSValue compiled = JS_Eval(a, compiled_source, strlen(compiled_source), "barrier-compiled", JS_EVAL_FLAG_COMPILE_ONLY);
        const char *module_source = "import 'barrier-native';hit()";
        JS_SetModuleLoaderFunc(barrier_engine, NULL, barrier_module_loader, NULL);
        JSValue module = JS_Eval(a, module_source, strlen(module_source), "barrier-module", JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY | JS_EVAL_FLAG_NO_RESOLVE);
        check(!JS_IsException(compiled) && !JS_IsException(module), "retained compiled inputs prepared before fencing");
        execution_eval(a, "globalThis.constructorProbe=function(){hit()}");
        JSValue ctor = realm_property(a, "constructorProbe");
        execution_eval(a, "globalThis.suspended=(function*(){yield 1;hit()})();suspended.next()");
        JSValue generator = realm_property(a, "suspended");
        JSValue next = JS_GetPropertyStr(a, generator, "next");
        JSClassExoticMethods exotic_methods = {
            .get_own_property = barrier_exotic_own, .get_own_property_names = barrier_exotic_names,
            .delete_property = barrier_exotic_delete, .define_own_property = barrier_exotic_define,
            .has_property = barrier_exotic_has, .get_property = barrier_exotic_get,
            .set_property = barrier_exotic_set, .get_prototype = barrier_exotic_proto,
            .set_prototype = barrier_exotic_set_proto, .is_extensible = barrier_exotic_extensible,
            .prevent_extensions = barrier_exotic_prevent
        };
        JSClassID exotic_class = os64_js_class_id(&barrier_exotic_class_id);
        JSClassDef exotic_def = {.class_name="BarrierExotic", .exotic=&exotic_methods};
        check(JS_NewClass(barrier_engine, exotic_class, &exotic_def) == 0, "frame-free exotic hooks registered");
        JSValue exotic = JS_NewObjectProtoClass(a, JS_NULL, exotic_class);
        JSClassID cleanup_class = os64_js_class_id(&barrier_cleanup_class_id);
        JSClassDef cleanup_def = {.class_name="BarrierCleanup", .finalizer=barrier_finalizer, .gc_mark=barrier_gc_mark};
        check(JS_NewClass(barrier_engine, cleanup_class, &cleanup_def) == 0, "cleanup hooks registered");
        JSValue drop = JS_NewObjectClass(a, cleanup_class), cycle = JS_NewObjectClass(a, cleanup_class);
        JS_SetPropertyStr(a, cycle, "self", JS_DupValue(a, cycle));
        JSValue buffer = JS_NewArrayBuffer(a, os64_malloc(8), 8, barrier_buffer_free, NULL, false);
        JSAtom key = JS_NewAtom(a, "barrierMissing");
        check(JS_EnqueueJob(a, barrier_job, 0, NULL) == 0, "pending job prepared before fence");
        JSJobInfo head, after;
        check(JS_PeekPendingJob(barrier_engine, &head), "queue provenance available before fence");
        JSValue marker = JS_NewError(a);
        JS_Throw(a, JS_DupValue(a, marker)); JS_SetUncatchableException(a, true);
        size_t allocations = attempts;
        check(JS_BeginRecoveryBarrier(barrier_engine, &barrier_token) && barrier_token.runtime == barrier_engine && barrier_token.generation,
              "idle tracked engine admits generation-bound fence without consuming exception");
        check(attempts == allocations && JS_HasException(a), "begin fence allocates nothing and preserves pending abort");
        JSRecoveryBarrier token = barrier_token;
        untouched = (JSRecoveryBarrier){NULL,77};
        check(!JS_BeginRecoveryBarrier(barrier_engine, &untouched) && untouched.generation == 77,
              "nested begin is refused without changing the token");
        JSRuntime *other = JS_NewRuntime();
        JS_EnableExecutionScopeTracking(other);
        JSRecoveryBarrier other_token;
        check(JS_BeginRecoveryBarrier(other, &other_token) && other_token.generation == token.generation,
              "independent runtime begins its own matching-number generation");
        check(!JS_EndRecoveryBarrier(other, &token) && JS_GetExecutionState(other, &state) && state.blocked &&
              JS_EndRecoveryBarrier(other, &other_token), "runtime identity prevents foreign same-generation release");
        JS_FreeRuntime(other);
        JSRecoveryBarrier forged = token; forged.generation++;
        check(!JS_EndRecoveryBarrier(barrier_engine, &forged), "wrong generation cannot release the fence");
        allocations = attempts;
        barrier_refused(a, JS_Call(a, healthy, JS_UNDEFINED, 0, NULL));
        barrier_refused(a, JS_Call(a, barrier_function, JS_UNDEFINED, 0, NULL));
        barrier_refused(a, JS_Call(a, foreign, JS_UNDEFINED, 0, NULL));
        barrier_refused(a, JS_CallConstructor(a, ctor, 0, NULL));
        barrier_refused(a, JS_Call(a, next, generator, 0, NULL));
        barrier_refused(a, JS_Eval(a, "hit()", 5, "fenced-eval", JS_EVAL_TYPE_GLOBAL));
        barrier_refused(a, JS_EvalFunction(a, JS_DupValue(a, compiled)));
        barrier_refused(a, JS_EvalFunction(a, JS_DupValue(a, module)));
        check(JS_ResolveModule(a, module) < 0, "module resolution refuses before loader/normalizer side effects");
        JSContext *job_ctx = a;
        check(JS_ExecutePendingJob(barrier_engine, &job_ctx) < 0 && job_ctx == NULL &&
              JS_PeekPendingJob(barrier_engine, &after) && after.context == head.context &&
              after.owner == head.owner && after.activity == head.activity && !barrier_jobs,
              "blocked job execution preserves queue head and provenance");
        value = JS_NewInt32(a, 23);
        check(!JS_RunWithSettings(a, a, barrier_settings, NULL, &value) && JS_VALUE_GET_INT(value) == 23,
              "fenced settings entry preserves output without invoking native body");
        JS_FreeValue(a, value);
        check(attempts == allocations, "fenced executable entry attempts no allocation");
        check(!barrier_effects && !barrier_loader_hits, "fenced executable entry performs no author/native body side effects");
        JSValue read = JS_GetProperty(a, exotic, key); JS_FreeValue(a, read);
        barrier_exotic_throws = true;
        read = JS_GetProperty(a, exotic, key);
        check(JS_IsException(read), "native callback exception propagates without losing its activity marker cleanup");
        JS_FreeValue(a, read); barrier_exotic_throws = false;
        check(JS_SetProperty(a, exotic, key, JS_UNDEFINED) >= 0 && JS_HasProperty(a, exotic, key) == 0 &&
              JS_DeleteProperty(a, exotic, key, 0) >= 0, "plain host exotic operations retain their ordinary contracts");
        JSPropertyEnum *names; uint32_t count;
        check(JS_GetOwnPropertyNames(a, &names, &count, exotic, JS_GPN_STRING_MASK) == 0 && count == 0,
              "host property enumeration callback is fenced against nested script entry");
        JS_FreePropertyEnum(a, names, count);
        read = JS_GetPrototype(a, exotic); JS_FreeValue(a, read);
        check(JS_SetPrototype(a, exotic, JS_NULL) >= 0 && JS_IsExtensible(a, exotic) == 1 &&
              JS_PreventExtensions(a, exotic) >= 0, "prototype/extensibility callouts retain their results");
        check(JS_DefineProperty(a, exotic, key, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, 0) >= 0,
              "define-property callout remains under native exclusion");
        JSPropertyDescriptor desc;
        check(JS_GetOwnProperty(a, &desc, exotic, key) == 0, "descriptor hook remains under native exclusion");
        JS_MarkValue(barrier_engine, exotic, barrier_mark_function);
        JSPrintValueOptions options; JS_PrintValueSetDefaultOptions(&options);
        JS_PrintValueRT(barrier_engine, barrier_print, barrier_engine, JS_NewInt32(a, 1), &options);
        JS_SetGlobalMissHandler(a, barrier_global_miss, barrier_engine);
        global = JS_GetGlobalObject(a); read = JS_GetProperty(a, global, key);
        JS_FreeValue(a, read); JS_FreeValue(a, global);
        JS_SetGlobalMissHandler(a, NULL, NULL);
        JS_DetachArrayBuffer(a, buffer); JS_FreeValue(a, buffer);
        JS_FreeValue(a, drop); JS_FreeValue(a, cycle); JS_RunGC(barrier_engine);
        check(JS_GetExecutionState(barrier_engine, &state) && state.blocked && !state.scopes &&
              !state.interpreter && !state.native_callbacks && !state.settings && !state.gc,
              "all engine activity markers restore after nested native cleanup");
        check(JS_MarkContextExecutableFailed(b), "idle fence owner can contain a child without executing it");
        JSValue pending = JS_GetException(a);
        check(JS_VALUE_GET_PTR(pending) == JS_VALUE_GET_PTR(marker), "blocked execution and cleanup preserve pending exception identity");
        JS_FreeValue(a, pending); JS_FreeValue(a, marker); JS_SetUncatchableException(a, false);
        allocations = attempts;
        check(JS_EndRecoveryBarrier(barrier_engine, &token) && !JS_EndRecoveryBarrier(barrier_engine, &token) &&
              attempts == allocations, "matching owner release is allocation-free and cannot be repeated");
        check(JS_BeginRecoveryBarrier(barrier_engine, &barrier_token) && barrier_token.generation > token.generation &&
              !JS_EndRecoveryBarrier(barrier_engine, &token) && JS_EndRecoveryBarrier(barrier_engine, &barrier_token),
              "later fence rejects stale generation from an earlier cycle");
        value = JS_Call(a, healthy, JS_UNDEFINED, 0, NULL); check(!JS_IsException(value), "healthy raw entry works after owner releases fence");
        JS_FreeValue(a, value);
        unsigned effects_before_resume = barrier_effects;
        value = JS_Call(a, next, generator, 0, NULL);
        check(!JS_IsException(value) && barrier_effects == effects_before_resume + 1,
              "blocked generator resumes in its original state after fence release");
        JS_FreeValue(a, value);
        value = JS_Call(a, foreign, JS_UNDEFINED, 0, NULL); check(JS_IsException(value), "failed child remains sealed after fence release");
        if (JS_IsException(value)) JS_FreeValue(a, JS_GetException(a));
        JS_FreeValue(a, value);
        check(JS_ExecutePendingJob(barrier_engine, NULL) == 1 && barrier_jobs == 1 && barrier_job_order[0] == 0,
              "owner release preserves queued work for its original dispatcher");
        barrier_expect_resolve_scope = true;
        check(JS_ResolveModule(a, module) == 0 && barrier_loader_hits == 1,
              "module resolution outside fence executes loader under activity markers");
        barrier_expect_resolve_scope = false;
        value = JS_EvalFunction(a, JS_DupValue(a, module));
        check(!JS_IsException(value), "native module initializer runs outside fence under activity markers");
        JS_FreeValue(a, value);
        for (unsigned i = 0; i < 16 && JS_IsJobPending(barrier_engine); i++) {
            int result = JS_ExecutePendingJob(barrier_engine, NULL);
            check(result >= 0, "module completion jobs drain outside fence");
            if (result < 0) JS_DiscardPendingJob(barrier_engine);
        }
        check(!JS_IsJobPending(barrier_engine), "fixture cleanup establishes bounded queue exhaustion");
        check(barrier_jobs == 3 && barrier_job_order[1] == 1 && barrier_job_order[2] == 1,
              "cleanup-enqueued jobs preserve the original queue order until owner release");
        check(barrier_probes > 20 && barrier_effects > 0, "native hook matrix executes only through permitted operations");
        JS_FreeAtom(a, key); JS_FreeValue(a, exotic); JS_FreeValue(a, ctor); JS_FreeValue(a, next);
        JS_FreeValue(a, generator); JS_FreeValue(a, module); JS_FreeValue(a, compiled);
        JS_FreeValue(a, healthy); JS_FreeValue(b, foreign); JS_FreeValue(a, barrier_function);
        os64_js_teardown_report_t report;
        os64_js_destroy_report(runtime, &report);
        check(!report.leaked && live == initial, "barrier tokens, consumed inputs and cleanup release all roots under selected teardown policy");
        os64_js_realm_t *realms[3] = {0}; JSContext *contexts[3] = {0};
        runtime = record_create(policy, realms, contexts);
        if (!runtime) continue;
        JSValue loop = record_function(contexts[1], "(function(){while(true){}})");
        record_call_timeout(realms[1], realms[1], loop, &out);
        check(os64_js_task_end(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_LIMIT,
              "real failed task closes before engine fence begins");
        os64_js_failure_record_t original, current;
        record_observe(runtime, &original);
        JSRuntime *engine = JS_GetRuntime(contexts[0]);
        JSRecoveryBarrier failed_token;
        check(JS_BeginRecoveryBarrier(engine, &failed_token) && JS_MarkContextExecutableFailed(contexts[1]) &&
              JS_EndRecoveryBarrier(engine, &failed_token), "engine-only containment fence can complete after real unwind");
        record_observe(runtime, &current); record_original_same(&original, &current);
        check(os64_js_realm_task_begin(realms[2], OS64_JS_ABI_ID, "still-terminal", &out) == OS64_JS_FAILED_RUNTIME &&
              os64_js_quiescent(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_FAILED_RUNTIME,
              "engine fence release grants no wrapper recovery or new sibling budget");
        JS_FreeValue(contexts[1], loop); record_close(runtime, initial);
    }
}
