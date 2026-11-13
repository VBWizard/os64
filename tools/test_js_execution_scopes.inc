/* Raw-engine observations: these facts do not enable wrapper recovery. */
typedef struct {
    JSExecutionScopeInfo items[96];
    size_t count;
    bool overflow;
} ExecutionSnapshot;
static ExecutionSnapshot execution_snapshot;
static unsigned execution_captures;
static JSContext *execution_interrupt_target;
static bool execution_interrupt_nested;

static void execution_visit(void *opaque, const JSExecutionScopeInfo *info)
{
    ExecutionSnapshot *snapshot = opaque;
    if (snapshot->count < sizeof(snapshot->items) / sizeof(snapshot->items[0]))
        snapshot->items[snapshot->count++] = *info;
    else
        snapshot->overflow = true;
}
static bool execution_has(JSExecutionScopeKind kind, JSContext *code)
{
    for (size_t i = 0; i < execution_snapshot.count; ++i)
        if (execution_snapshot.items[i].kind == kind && execution_snapshot.items[i].code == code)
            return true;
    return false;
}
static void execution_take(JSRuntime *runtime)
{
    size_t before = attempts;
    memset(&execution_snapshot, 0, sizeof(execution_snapshot));
    check(JS_VisitExecutionScopes(runtime, execution_visit, &execution_snapshot),
          "scope snapshot reports tracking enabled");
    check(attempts == before && !execution_snapshot.overflow,
          "scope snapshot captures without allocating or truncating");
}
static JSValue execution_capture(JSContext *context, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self; (void)argc; (void)argv;
    execution_take(JS_GetRuntime(context));
    ++execution_captures;
    return JS_NewInt32(context, 37);
}
static JSValue execution_capture_data(JSContext *context, JSValueConst self, int argc,
                                       JSValueConst *argv, int magic, JSValue *data)
{
    (void)magic; (void)data;
    return execution_capture(context, self, argc, argv);
}
static JSValue execution_reenter(JSContext *context, JSValueConst self, int argc,
                                  JSValueConst *argv, int magic, JSValue *data)
{
    (void)self; (void)argc; (void)argv; (void)magic;
    return JS_Call(context, data[0], JS_UNDEFINED, 0, NULL);
}
static JSValue execution_job(JSContext *context, int argc, JSValueConst *argv)
{
    return execution_capture(context, JS_UNDEFINED, argc, argv);
}
static void execution_eval(JSContext *context, const char *source)
{
    JSValue result = JS_Eval(context, source, strlen(source), "scope-test.js", JS_EVAL_TYPE_GLOBAL);
    check(!JS_IsException(result), "scope fixture source executes");
    if (JS_IsException(result)) JS_FreeValue(context, JS_GetException(context));
    JS_FreeValue(context, result);
}
static void execution_idle(JSRuntime *runtime)
{
    execution_take(runtime);
    check(execution_snapshot.count == 0, "scope chain is empty after engine unwind");
}
static int execution_interrupt(JSRuntime *runtime, void *opaque)
{
    (void)opaque;
    ExecutionSnapshot snapshot = {0};
    JS_VisitExecutionScopes(runtime, execution_visit, &snapshot);
    if (!execution_interrupt_nested ||
        (snapshot.count && snapshot.items[0].kind == JS_EXECUTION_SCOPE_CALL &&
         snapshot.items[0].code == execution_interrupt_target)) {
        execution_take(runtime);
        return 1;
    }
    return 0;
}
static void execution_scope_cases(void)
{
    size_t initial_live = live;
    JSRuntime *runtime = JS_NewRuntime();
    JSContext *a = runtime ? JS_NewContext(runtime) : NULL;
    JSContext *b = runtime ? JS_NewContext(runtime) : NULL;
    JSContext *c = runtime ? JS_NewContext(runtime) : NULL;
    check(a && b && c, "scope fixture creates three raw contexts");
    if (!a || !b || !c) goto done;
    ExecutionSnapshot disabled = {0};
    check(!JS_VisitExecutionScopes(runtime, execution_visit, &disabled) && disabled.count == 0,
          "scope tracking is disabled by default");
    JS_EnableExecutionScopeTracking(runtime);
    execution_idle(runtime);
    JSValue global = JS_GetGlobalObject(b);
    JSValue native = JS_NewCFunction(c, execution_capture, "capture", 0);
    check(JS_SetPropertyStr(b, global, "capture", native) == 1, "foreign native probe installed");
    native = JS_NewCFunctionData(c, execution_capture_data, 0, 0, 0, NULL);
    check(JS_SetPropertyStr(b, global, "captureData", native) == 1, "foreign data probe installed");
    JS_FreeValue(b, global);
    execution_eval(b, "function bFn(){return capture()} function bData(){return captureData()}");
    realm_share(b, a, "bFn"); realm_share(b, a, "bData");
    execution_captures = 0;
    execution_eval(a, "function aFn(){return bFn()} if(aFn()!==37)throw 1");
    check(execution_captures == 1 && execution_has(JS_EXECUTION_SCOPE_CALL, a) &&
          execution_has(JS_EXECUTION_SCOPE_CALL, b), "snapshot contains both raw caller bytecode realms");
    check(execution_snapshot.items[0].caller == b && execution_snapshot.items[0].code == c &&
          execution_snapshot.items[0].code_kind == JS_EXECUTION_CODE_NATIVE,
          "native execution scope separates incoming context from native definition realm");
    execution_eval(a, "if(bData()!==37)throw 1");
    check(execution_snapshot.items[0].caller == b && execution_snapshot.items[0].code == c &&
          execution_snapshot.items[0].code_kind == JS_EXECUTION_CODE_NATIVE,
          "native data callback records defining realm even with caller context argument");
    execution_idle(runtime);
    JSValue reentry_target = realm_property(b, "bFn");
    native = JS_NewCFunctionData(c, execution_reenter, 0, 0, 1, &reentry_target);
    JS_FreeValue(b, reentry_target);
    global = JS_GetGlobalObject(a);
    check(JS_SetPropertyStr(a, global, "nativeReentry", native) == 1, "raw native reentry probe installed");
    JS_FreeValue(a, global);
    execution_eval(a, "if(nativeReentry()!==37)throw 1");
    check(execution_has(JS_EXECUTION_SCOPE_CALL, a) && execution_has(JS_EXECUTION_SCOPE_CALL, b) &&
          execution_has(JS_EXECUTION_SCOPE_CALL, c), "raw engine reentry retains caller, native and target scopes");
    execution_idle(runtime);

    execution_eval(b, "class Ctor{constructor(){capture()}};new Ctor");
    check(execution_has(JS_EXECUTION_SCOPE_CONSTRUCT, b), "constructor dispatch has an independent scope");
    execution_eval(b, "let obj={get x(){return capture()},valueOf(){return capture()}};obj.x;+obj");
    check(execution_has(JS_EXECUTION_SCOPE_CALL, b), "coercion and accessor code enter tracked scopes");
    execution_eval(b, "bFn.bind(null)()");
    check(execution_has(JS_EXECUTION_SCOPE_CALL, b), "bound target preserves executable code realm");
    execution_eval(b, "let deep=bFn;for(let i=0;i<66;i++)deep=deep.bind(null);deep()");
    bool deep_unknown = false;
    for (size_t i = 0; i < execution_snapshot.count; ++i)
        deep_unknown |= execution_snapshot.items[i].code == NULL &&
                        execution_snapshot.items[i].code_kind == JS_EXECUTION_CODE_UNKNOWN;
    check(deep_unknown && execution_has(JS_EXECUTION_SCOPE_CALL, b),
          "deep bound dispatch remains unknown until bounded discovery reaches the code");
    execution_eval(b, "new Proxy(bFn,{apply(t,s,args){return Reflect.apply(t,s,args)}})()");
    bool unknown = false;
    for (size_t i = 0; i < execution_snapshot.count; ++i)
        unknown |= execution_snapshot.items[i].kind == JS_EXECUTION_SCOPE_CALL &&
                   execution_snapshot.items[i].code == NULL &&
                   execution_snapshot.items[i].code_kind == JS_EXECUTION_CODE_UNKNOWN;
    check(unknown && execution_has(JS_EXECUTION_SCOPE_CALL, b),
          "proxy dispatch remains unknown alongside observed handler and target execution");
    execution_eval(b, "eval('capture()')");
    check(execution_has(JS_EXECUTION_SCOPE_EVAL, b), "direct eval records its compilation environment");
    execution_eval(b, "function* gen(){capture();yield 1} gen().next()");
    check(execution_has(JS_EXECUTION_SCOPE_RESUME, b), "generator resume records suspended code realm");
    execution_idle(runtime);

    execution_eval(b, "Promise.resolve().then(()=>capture())");
    JSContext *job_context = NULL;
    unsigned visits = 0;
    while (JS_IsJobPending(runtime) && visits++ < 30) {
        check(JS_ExecutePendingJob(runtime, &job_context) >= 0, "owned Promise job completes");
    }
    check(!JS_IsJobPending(runtime) && execution_has(JS_EXECUTION_SCOPE_JOB, b),
          "Promise callback scope retains job owner independently of dispatch");
    check(JS_EnqueueJobOwned(b, NULL, a, execution_job, 0, NULL) == 0,
          "neutral job with separate activity owner queued");
    check(JS_ExecutePendingJob(runtime, &job_context) == 1 &&
          execution_snapshot.items[0].kind == JS_EXECUTION_SCOPE_JOB &&
          execution_snapshot.items[0].caller == b && execution_snapshot.items[0].code == NULL &&
          execution_snapshot.items[0].code_kind == JS_EXECUTION_CODE_UNKNOWN,
          "neutral native job does not invent code ownership from activity context");
    execution_idle(runtime);
    execution_eval(b, "async function af(){await 1;return capture()} af()");
    visits = 0;
    while (JS_IsJobPending(runtime) && visits++ < 30)
        check(JS_ExecutePendingJob(runtime, &job_context) >= 0, "async resume job completes");
    check(!JS_IsJobPending(runtime) && execution_has(JS_EXECUTION_SCOPE_RESUME, b),
          "async continuation resumes with the suspended bytecode environment");
    execution_eval(b, "async function* ag(){await 1;capture();yield 1} ag().next()");
    visits = 0;
    while (JS_IsJobPending(runtime) && visits++ < 30)
        check(JS_ExecutePendingJob(runtime, &job_context) >= 0, "async generator job completes");
    check(!JS_IsJobPending(runtime) && execution_has(JS_EXECUTION_SCOPE_RESUME, b),
          "async generator retains its resumed bytecode scope");
    execution_idle(runtime);
    JSValue thrown = JS_Eval(a, "try{(()=>{throw 1})()}catch(e){}", strlen("try{(()=>{throw 1})()}catch(e){}"), "scope-throw.js", JS_EVAL_TYPE_GLOBAL);
    check(!JS_IsException(thrown), "ordinary caught exception unwinds its scope");
    JS_FreeValue(a, thrown);
    execution_idle(runtime);

    JSValue module = JS_Eval(b, "capture();export default 1", strlen("capture();export default 1"), "scope-module.js",
                            JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    check(!JS_IsException(module), "scope module compiled");
    JSValue module_result = JS_EvalFunction(b, module);
    check(!JS_IsException(module_result) && execution_has(JS_EXECUTION_SCOPE_EVAL_FUNCTION, b),
          "compiled module execution has a separate scope");
    JS_FreeValue(b, module_result);
    visits = 0;
    while (JS_IsJobPending(runtime) && visits++ < 30)
        check(JS_ExecutePendingJob(runtime, &job_context) >= 0, "module jobs complete");
    execution_idle(runtime);

    execution_eval(b, "globalThis.effects=0;globalThis.target=()=>{effects++;return 11}");
    JSValue target = realm_property(b, "target");
    execution_interrupt_target = b; execution_interrupt_nested = false;
    JS_SetInterruptHandler(runtime, execution_interrupt, NULL);
    bool interrupted = false;
    unsigned successes = 0;
    for (unsigned i = 0; i < 20000; ++i) {
        JSValue result = JS_Call(a, target, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(result)) {
            interrupted = true;
            JS_FreeValue(a, JS_GetException(a));
            break;
        }
        ++successes;
        JS_FreeValue(a, result);
    }
    JS_SetInterruptHandler(runtime, NULL, NULL);
    check(interrupted && execution_snapshot.count == 1 &&
          execution_snapshot.items[0].caller == a && execution_snapshot.items[0].code == b,
          "pre-entry interrupt observes pending child before any interpreter frame exists");
    JSValue effects = realm_property(b, "effects");
    int32_t effect_count = -1;
    JS_ToInt32(b, &effect_count, effects);
    check(effect_count == (int32_t)successes, "interrupted pending callee performs no body side effect");
    JS_FreeValue(b, effects); JS_FreeValue(b, target);
    execution_idle(runtime);

    execution_eval(c, "globalThis.cSpin=()=>{while(true){}}");
    realm_share(c, b, "cSpin");
    execution_eval(b, "globalThis.bSpin=()=>cSpin()");
    realm_share(b, a, "bSpin");
    execution_interrupt_target = c; execution_interrupt_nested = true;
    JS_SetInterruptHandler(runtime, execution_interrupt, NULL);
    JSValue result = JS_Eval(a, "bSpin()", 7, "scope-unwind.js", JS_EVAL_TYPE_GLOBAL);
    JS_SetInterruptHandler(runtime, NULL, NULL);
    check(JS_IsException(result) && execution_has(JS_EXECUTION_SCOPE_CALL, a) &&
          execution_has(JS_EXECUTION_SCOPE_CALL, b) && execution_has(JS_EXECUTION_SCOPE_CALL, c),
          "interrupt captures all three abandoned code environments");
    if (JS_IsException(result)) JS_FreeValue(a, JS_GetException(a));
    JS_FreeValue(a, result);
    execution_idle(runtime);

done:
    if (a) JS_FreeContext(a);
    if (b) JS_FreeContext(b);
    if (c) JS_FreeContext(c);
    if (runtime) JS_FreeRuntime(runtime);
    check(live == initial_live, "scope fixture releases every raw engine allocation");
}
