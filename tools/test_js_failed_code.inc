/* Trusted raw-engine containment tests; wrapper recovery is not enabled. */
static unsigned failed_native_hits, failed_job_hits, failed_loader_hits;
static JSContext *failed_mark_target;
static JSModuleDef *failed_import_module;
static JSValue failed_native(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self; (void)argc; (void)argv;
    failed_native_hits++;
    return JS_NewInt32(ctx, 5);
}
static JSValue failed_native_data(JSContext *ctx, JSValueConst self, int argc,
                                 JSValueConst *argv, int magic, JSValue *data)
{
    (void)magic; (void)data;
    return failed_native(ctx, self, argc, argv);
}
static JSValue failed_active_mark(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)ctx; (void)self; (void)argc; (void)argv;
    check(!JS_MarkContextExecutableFailed(failed_mark_target) &&
          !JS_IsContextExecutableFailed(failed_mark_target),
          "executable containment refuses marking beneath native/engine entry");
    return JS_UNDEFINED;
}
static JSValue failed_job(JSContext *ctx, int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    failed_job_hits++;
    return JS_UNDEFINED;
}
static int failed_module_init(JSContext *ctx, JSModuleDef *module)
{
    (void)ctx; (void)module;
    failed_native_hits++;
    return 0;
}
static JSModuleDef *failed_module_loader(JSContext *ctx, const char *name, void *opaque)
{
    (void)ctx; (void)opaque;
    failed_loader_hits++;
    return !strcmp(name, "failed-native") ? failed_import_module : NULL;
}
static JSValue failed_custom_call(JSContext *ctx, JSValueConst function, JSValueConst self,
                                  int argc, JSValueConst *argv, int flags)
{
    (void)function; (void)flags;
    return failed_native(ctx, self, argc, argv);
}
static void failed_eval(JSContext *ctx, const char *source, bool refuse, const char *name)
{
    JSValue result = JS_Eval(ctx, source, strlen(source), "failed-code.js", JS_EVAL_TYPE_GLOBAL);
    check(JS_IsException(result) == refuse, name);
    if (JS_IsException(result)) {
        JSValue error = JS_GetException(ctx);
        check(JS_IsError(ctx, error), "failed entry returns an engine error rather than executing a body");
        JS_FreeValue(ctx, error);
        JS_SetUncatchableException(ctx, false);
    }
    JS_FreeValue(ctx, result);
}
static bool failed_unwind_armed;
static JSValue failed_unwind_ready(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)ctx; (void)self; (void)argc; (void)argv;
    failed_unwind_armed = true;
    return JS_UNDEFINED;
}
static int failed_unwind_interrupt(JSRuntime *runtime, void *opaque)
{
    return failed_unwind_armed ? execution_interrupt(runtime, opaque) : 0;
}
static void failed_unwind_cases(void)
{
    size_t before = live;
    JSRuntime *runtime = JS_NewRuntime();
    JSContext *a = runtime ? JS_NewContext(runtime) : NULL;
    JSContext *b = runtime ? JS_NewContext(runtime) : NULL;
    JSContext *c = runtime ? JS_NewContext(runtime) : NULL;
    check(a && b && c, "post-interrupt alias fixture creates three contexts");
    if (!a || !b || !c) goto done;
    JS_EnableExecutionScopeTracking(runtime);
    execution_interrupt_target = b;
    execution_interrupt_nested = true;
    failed_unwind_armed = false;
    JSValue global = JS_GetGlobalObject(b);
    check(JS_SetPropertyStr(b, global, "ready", JS_NewCFunction(b, failed_unwind_ready, "ready", 0)) == 1,
          "post-interrupt fixture arms only after exporting its closure and data");
    JS_FreeValue(b, global);
    JS_SetInterruptHandler(runtime, failed_unwind_interrupt, NULL);
    const char *source = "globalThis.box={value:41};(function(){let n=7;globalThis.closedB=()=>++n;"
                         "box.value=42;ready();try{while(true){}}finally{box.value=99}})()";
    JSValue result = JS_Eval(b, source, strlen(source), "failed-unwind.js", JS_EVAL_TYPE_GLOBAL);
    JS_SetInterruptHandler(runtime, NULL, NULL);
    check(JS_IsException(result) && execution_has(JS_EXECUTION_SCOPE_CALL, b),
          "real interruption unwinds child stack containing an exported closure and data");
    if (JS_IsException(result)) JS_FreeValue(b, JS_GetException(b));
    JS_FreeValue(b, result);
    realm_share(b, a, "box"); realm_share(b, a, "closedB"); realm_share(b, c, "box");
    check(JS_MarkContextExecutableFailed(b), "interrupted context seals after complete engine unwind");
    failed_eval(a, "let refused=false;try{closedB()}catch(e){refused=true}"
                   "if(!refused||box.value!==42)throw 1;box.value=43", false,
                   "closed stack references remain retained while failed closure entry is refused");
    failed_eval(c, "if(box.value!==43)throw 1;box.value=44", false,
                   "third context sees retained data mutations after child-only engine containment");
    execution_idle(runtime);
done:
    if (a) JS_FreeContext(a);
    if (b) JS_FreeContext(b);
    if (c) JS_FreeContext(c);
    if (runtime) JS_FreeRuntime(runtime);
    check(live == before, "post-interrupt closures and retained data release all engine allocations");
}
static void failed_wrapper_cases(void)
{
    for (unsigned policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; ++policy) {
        size_t before = live;
        os64_js_config_t config = fixture_config();
        os64_js_runtime_t *runtime = NULL;
        os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config, policy, OS64_JS_ABI_ID, &runtime, &out) == OS64_JS_OK,
              "failed alias wrapper fixture created under selected teardown policy");
        if (!runtime) continue;
        os64_js_realm_t *a = os64_js_primary_realm(runtime, OS64_JS_ABI_ID, &out), *b = NULL;
        check(os64_js_realm_create(runtime, OS64_JS_ABI_ID, &b, &out) == OS64_JS_OK,
              "failed alias wrapper child created");
        if (!b) { os64_js_destroy(runtime); continue; }
        JSContext *ac = os64_js_realm_context(a, OS64_JS_ABI_ID, &out);
        JSContext *bc = os64_js_realm_context(b, OS64_JS_ABI_ID, &out);
        JS_EnableExecutionScopeTracking(JS_GetRuntime(ac));
        check(realm_execute(b, "globalThis.f=()=>3;globalThis.o={x:41}", &out) == OS64_JS_OK,
              "failed alias wrapper values prepared");
        realm_share(bc, ac, "f"); realm_share(bc, ac, "o");
        check(os64_js_realm_retire(b, OS64_JS_ABI_ID, &out) == OS64_JS_OK &&
              realm_execute(a, "if(f()!==3)throw 1", &out) == OS64_JS_OK,
              "ordinary departure still permits retained function execution");
        check(JS_MarkContextExecutableFailed(bc), "explicit engine failure mark seals departed child");
        check(realm_execute(a, "f()", &out) == OS64_JS_EXCEPTION,
              "wrapper reports failed alias as local script exception");
        check(realm_execute(a, "let caught=false;try{f()}catch(e){caught=true}if(!caught||o.x!==41)throw 1;o.x=42", &out) == OS64_JS_OK,
              "healthy wrapper remains reusable and retained data remains mutable");
        os64_js_teardown_report_t report;
        os64_js_destroy_report(runtime, &report);
        check(!report.leaked && live == before,
              "failed alias wrapper teardown releases roots under selected policy");
    }
}
static void failed_code_cases(void)
{
    size_t before = live;
    JSRuntime *runtime = JS_NewRuntime();
    JSContext *a = runtime ? JS_NewContext(runtime) : NULL;
    JSContext *b = runtime ? JS_NewContext(runtime) : NULL;
    JSContext *c = runtime ? JS_NewContext(runtime) : NULL;
    check(a && b && c, "failed-code fixture creates three contexts");
    if (!a || !b || !c) goto done;
    check(!JS_MarkContextExecutableFailed(b) && !JS_IsContextExecutableFailed(b),
          "failure marking requires explicit scope tracking");
    JS_EnableExecutionScopeTracking(runtime);
    failed_mark_target = b;
    JSValue global = JS_GetGlobalObject(a);
    check(JS_SetPropertyStr(a, global, "tryMark", JS_NewCFunction(a, failed_active_mark, "tryMark", 0)) == 1,
          "active failure-mark probe installed");
    JS_FreeValue(a, global);
    failed_eval(a, "tryMark();globalThis.shared={hits:0,traps:0,healthy:0};"
                  "globalThis.holdA=new Promise(r=>globalThis.releaseA=r);"
                  "globalThis.healthyFn=()=>{shared.healthy++;return 9}", false, "healthy fixture prepared");
    realm_share(a,b,"shared"); realm_share(a,c,"shared"); realm_share(a,b,"holdA");
    failed_eval(b,
        "globalThis.fnB=function(){shared.hits++;return 3};"
        "globalThis.boundB=fnB.bind(null);globalThis.deepB=fnB;"
        "for(let i=0;i<66;i++)deepB=deepB.bind(null);"
        "globalThis.handlerB={apply(){shared.hits++;return 1},construct(){shared.hits++;return {}}};"
        "globalThis.lookupB={get apply(){shared.hits++;return fnB}};"
        "globalThis.objectProxyB=new Proxy({},{get(){shared.hits++},set(){shared.hits++;return true},"
        "has(){shared.hits++;return false},ownKeys(){shared.hits++;return []},"
        "getOwnPropertyDescriptor(){shared.hits++},defineProperty(){shared.hits++;return true},"
        "deleteProperty(){shared.hits++;return true},getPrototypeOf(){shared.hits++;return null},"
        "setPrototypeOf(){shared.hits++;return true},isExtensible(){shared.hits++;return true},"
        "preventExtensions(){shared.hits++;return false}});"
        "globalThis.dataB={value:41,get x(){shared.hits++;return 1},set x(v){shared.hits++},"
        "[Symbol.toPrimitive](){shared.hits++;return 1}};"
        "globalThis.CtorB=class{constructor(){shared.hits++}};"
        "globalThis.evalB=eval;globalThis.FunctionB=Function;"
        "globalThis.pB=new Promise(r=>globalThis.resolveB=r);"
        "globalThis.asyncB=(async()=>{await holdA;shared.hits++})();"
        "globalThis.importB=import('failed-native');"
        "globalThis.thenableB={then(r){shared.hits++;r(1)}};"
        "globalThis.gNext=(function*(){try{yield 0;shared.hits++}finally{shared.hits++}})();gNext.next();"
        "globalThis.gThrow=(function*(){try{yield 0;shared.hits++}finally{shared.hits++}})();gThrow.next();"
        "globalThis.gReturn=(function*(){try{yield 0;shared.hits++}finally{shared.hits++}})();gReturn.next();"
        "globalThis.agB=(async function*(){shared.hits++;yield 0})();"
        "Promise.resolve().then(()=>shared.hits++);",
        false, "failed environment aliases and suspended work prepared before marking");
    const char *names[]={"fnB","boundB","deepB","handlerB","lookupB","dataB","CtorB","evalB","FunctionB", "pB","resolveB","thenableB","objectProxyB","gNext","gThrow","gReturn","agB"};
    for (size_t i=0;i<sizeof(names)/sizeof(names[0]);i++) realm_share(b,a,names[i]);
    global=JS_GetGlobalObject(a);
    check(JS_SetPropertyStr(a,global,"nativeB",JS_NewCFunction(b,failed_native,"nativeB",0))==1,
          "foreign native alias installed");
    check(JS_SetPropertyStr(a,global,"dataNativeB",JS_NewCFunctionData(b,failed_native_data,0,0,0,NULL))==1 &&
          JS_SetPropertyStr(a,global,"neutralNativeB",JS_NewCFunctionDataNeutral(b,failed_native_data,0,0,0,NULL))==1,
          "foreign data and neutral aliases installed");
    JSClassID custom_class=0;
    JS_NewClassID(&custom_class);
    JSClassDef definition={.class_name="UnknownCallable",.call=failed_custom_call};
    check(JS_NewClass(runtime,custom_class,&definition)==0 &&
          JS_SetPropertyStr(a,global,"custom",JS_NewObjectClass(a,custom_class))==1,
          "unclassified native callable installed");
    JS_FreeValue(a,global);
    failed_eval(a,"globalThis.proxyTargetB=new Proxy(fnB,{apply(){shared.traps++;return 1}});"
                  "globalThis.proxyHandlerB=new Proxy(healthyFn,handlerB);"
                  "globalThis.proxyLookupB=new Proxy(healthyFn,lookupB);"
                  "globalThis.revocable=Proxy.revocable(fnB,{});revocable.revoke();"
                  "globalThis.healthyProxy=new Proxy(healthyFn,{apply(t,s,args){shared.traps++;return Reflect.apply(t,s,args)}});"
                  "globalThis.genMethods=Object.getPrototypeOf(Object.getPrototypeOf((function*(){})()));"
                  "globalThis.agenMethods=Object.getPrototypeOf(Object.getPrototypeOf((async function*(){})()));",
                  false,"healthy proxies and borrowed generator intrinsics prepared");
    JSValue bytecode=JS_Eval(b,"shared.hits++",strlen("shared.hits++"),"failed-cached.js",JS_EVAL_FLAG_COMPILE_ONLY);
    JSValue module=JS_Eval(b,"shared.hits++;export const x=1",strlen("shared.hits++;export const x=1"),
                          "failed-module.js",JS_EVAL_TYPE_MODULE|JS_EVAL_FLAG_COMPILE_ONLY|JS_EVAL_FLAG_NO_RESOLVE);
    check(!JS_IsException(bytecode)&&!JS_IsException(module),"retained compiled inputs prepared");
    const char *data_module_source="export const value=41";
    JSValue data_module=JS_Eval(b,data_module_source,strlen(data_module_source),"failed-data.js",
                              JS_EVAL_TYPE_MODULE|JS_EVAL_FLAG_COMPILE_ONLY);
    check(!JS_IsException(data_module),"retained data module compiled before marking");
    JSValue data_result=JS_EvalFunction(b,JS_DupValue(b,data_module));
    check(!JS_IsException(data_result),"retained data module evaluates before marking");
    JS_FreeValue(b,data_result);
    JSValue namespace=JS_GetModuleNamespace(b,JS_VALUE_GET_PTR(data_module));
    global=JS_GetGlobalObject(a);
    check(JS_SetPropertyStr(a,global,"nsB",namespace)==1,"retained module namespace shared before marking");
    JS_FreeValue(a,global);JS_FreeValue(b,data_module);
    const char *tla_source="await holdA;shared.hits++;export const ready=1";
    JSValue tla=JS_Eval(b,tla_source,strlen(tla_source),"failed-tla.js",JS_EVAL_TYPE_MODULE);
    check(!JS_IsException(tla),"pending top-level-await module prepared before failure mark");
    JS_FreeValue(b,tla);
    failed_import_module=JS_NewCModule(b,"failed-native",failed_module_init);
    check(failed_import_module!=NULL,"native failed-module dependency prepared");
    JS_SetModuleLoaderFunc(runtime,NULL,failed_module_loader,NULL);
    JSValue graph=JS_Eval(c,"import 'failed-native';shared.healthy++",strlen("import 'failed-native';shared.healthy++"),
                         "healthy-graph.js",JS_EVAL_TYPE_MODULE|JS_EVAL_FLAG_COMPILE_ONLY|JS_EVAL_FLAG_NO_RESOLVE);
    check(!JS_IsException(graph),"healthy graph importing failed dependency prepared");
    check(JS_EnqueueJobOwned(b,NULL,NULL,failed_job,0,NULL)==0 &&
          JS_EnqueueJobOwned(a,b,NULL,failed_job,0,NULL)==0 &&
          JS_EnqueueJobOwned(a,NULL,b,failed_job,0,NULL)==0,
          "native jobs with execution, callback and activity failure routes queued");
    failed_native_hits=failed_job_hits=failed_loader_hits=0;
    size_t allocations=attempts;
    check(JS_MarkContextExecutableFailed(b)&&JS_MarkContextExecutableFailed(b)&&
          JS_IsContextExecutableFailed(b)&&!JS_IsContextExecutableFailed(a)&&!JS_IsContextExecutableFailed(c),
          "failure mark is irreversible, idempotent and child-local");
    check(attempts==allocations,"failure mark allocates nothing and leaves queued work untouched");
    const char *blocked[]={"fnB()","fnB.call(null)","fnB.apply(null,[])","Reflect.apply(fnB,null,[])","fnB(...[])","boundB()","deepB()","proxyTargetB()","proxyHandlerB()","proxyLookupB()",
        "revocable.proxy()","dataB.x","dataB.x=2","+dataB","new CtorB","Reflect.construct(CtorB,[])",
        "new (new Proxy(CtorB,{construct(){shared.traps++;return {}}}))()",
        "evalB('shared.hits++')","(0,evalB)('shared.hits++')","new FunctionB('shared.hits++')","FunctionB('shared.hits++')","nativeB()","dataNativeB()","neutralNativeB()",
        "resolveB(1)","objectProxyB.x","objectProxyB.x=1","'x' in objectProxyB", "Reflect.ownKeys(objectProxyB)",
        "Object.getOwnPropertyDescriptor(objectProxyB,'x')","Object.defineProperty(objectProxyB,'x',{value:1})",
        "delete objectProxyB.x","Object.getPrototypeOf(objectProxyB)","Object.setPrototypeOf(objectProxyB,null)",
        "Object.isExtensible(objectProxyB)","Object.preventExtensions(objectProxyB)","genMethods.next.call(gNext)","genMethods.throw.call(gThrow,1)","genMethods.return.call(gReturn,1)","custom()"};
    for(size_t i=0;i<sizeof(blocked)/sizeof(blocked[0]);i++) failed_eval(a,blocked[i],true,blocked[i]);
    failed_eval(a,"let caught=false;try{fnB()}catch(e){caught=true}if(!caught)throw 1;"
                  "if(dataB.value!==41||dataB!==dataB||nsB.value!==41)throw 2;dataB.value=42;"
                  "if(healthyProxy()!==9)throw 3;",false,
                  "blocked aliases are catchable while healthy proxy and retained data remain usable");
    failed_eval(b,"shared.hits++",true,"failed context refuses new eval before parsing or execution");
    JSValue refused_compile=JS_Eval(b,"shared.hits++",strlen("shared.hits++"),"failed-compile.js",JS_EVAL_FLAG_COMPILE_ONLY);
    check(JS_IsException(refused_compile),"failed context refuses compile-only eval before producing bytecode");
    if(JS_IsException(refused_compile))JS_FreeValue(b,JS_GetException(b));
    JS_FreeValue(b,refused_compile);
    check(JS_ResolveModule(a,module)<0,"foreign-context module resolution cannot bypass failed owner");
    JS_FreeValue(a,JS_GetException(a));
    JSValue result=JS_EvalFunction(a,bytecode);
    check(JS_IsException(result),"foreign-context bytecode execution cannot bypass failed owner");
    JS_FreeValue(a,JS_GetException(a));JS_FreeValue(a,result);
    result=JS_EvalFunction(a,module);
    check(JS_IsException(result),"foreign-context module execution cannot bypass failed owner");
    JS_FreeValue(a,JS_GetException(a));JS_FreeValue(a,result);
    check(JS_ResolveModule(c,graph)<0 && failed_loader_hits==1 && failed_native_hits==0,
          "module graph refuses failed native dependency before its initializer");
    JS_FreeValue(c,JS_GetException(c));JS_FreeValue(c,graph);
    failed_eval(a,"releaseA(1);agenMethods.next.call(agB);Promise.resolve(thenableB);Promise.resolve().then(()=>shared.healthy++)",false,
                  "healthy caller can attempt suspended continuations without running failed code");
    unsigned visits=0;JSContext *job_context=NULL;
    while(JS_IsJobPending(runtime)&&visits++<100){
        int executed=JS_ExecutePendingJob(runtime,&job_context);
        if(executed<0)JS_FreeValue(a,JS_GetException(a));
    }
    check(!JS_IsJobPending(runtime)&&failed_native_hits==0&&failed_job_hits==0&&failed_loader_hits==1,
          "failed jobs, async continuations and neutral native bodies perform no side effects");
    failed_eval(a,"if(shared.hits!==0||shared.traps!==1||shared.healthy!==2||dataB.value!==42)throw shared",false,
                  "failed environments stay sealed while healthy work and shared data preserve values");
    /* The guard must not turn an existing emergency abort into a catchable
       alias error, including when native code tries another raw call. */
    JSValue token=JS_NewObject(a);
    JS_Throw(a,JS_DupValue(a,token));JS_SetUncatchableException(a,true);
    JSValue fn=realm_property(a,"fnB");
    result=JS_Call(a,fn,JS_UNDEFINED,0,NULL);
    JSValue retained=JS_GetException(a);
    check(JS_IsException(result)&&JS_VALUE_GET_PTR(retained)==JS_VALUE_GET_PTR(token),
          "blocked alias preserves the original uncatchable exception identity");
    JSValue alias_result=JS_Call(a,fn,JS_UNDEFINED,0,NULL);
    JSValue alias_error=JS_GetException(a);
    check(JS_IsException(alias_result)&&JS_IsError(a,alias_error),
          "consumed abort marker cannot suppress a subsequent local alias error");
    JS_FreeValue(a,alias_error);JS_FreeValue(a,alias_result);
    JS_FreeValue(a,retained);JS_FreeValue(a,result);
    JS_Throw(a,JS_DupValue(a,token));JS_SetUncatchableException(a,true);
    const char *abort_source="try{fnB()}catch(e){shared.hits++}finally{shared.hits++}";
    result=JS_Eval(a,abort_source,strlen(abort_source),"failed-abort.js",JS_EVAL_TYPE_GLOBAL);
    retained=JS_GetException(a);
    check(JS_IsException(result)&&JS_VALUE_GET_PTR(retained)==JS_VALUE_GET_PTR(token),
          "failed guard keeps an original abort uncatchable across author catch and finally");
    JS_SetUncatchableException(a,false);
    JS_FreeValue(a,retained);JS_FreeValue(a,token);JS_FreeValue(a,fn);JS_FreeValue(a,result);
    failed_eval(a,"if(shared.hits!==0)throw 1",false,"abort guard runs neither author catch nor finally");
    JSContext *detached=JS_NewContext(runtime);
    JSModuleDef *held=detached?JS_NewCModule(detached,"held-native",failed_module_init):NULL;
    check(held&&JS_MarkContextExecutableFailed(detached),"held native module has a marked creation context");
    if(held){
        JSValue held_value=JS_DupValue(a,JS_MKPTR(JS_TAG_MODULE,held));
        JS_FreeContext(detached);
        JS_RunGC(runtime);
        check(JS_IsContextExecutableFailed(detached),"externally held native module keeps its owner alive through GC");
        result=JS_EvalFunction(a,held_value);
        check(JS_IsException(result)&&failed_native_hits==0,"native module cannot evade its failed owner after context release");
        JS_FreeValue(a,JS_GetException(a));JS_FreeValue(a,result);
        JS_RunGC(runtime);
    } else if(detached) {
        JS_FreeContext(detached);
    }
    execution_idle(runtime);
done:
    if (a) JS_FreeContext(a);
    if (b) JS_FreeContext(b);
    if (c) JS_FreeContext(c);
    if (runtime) JS_FreeRuntime(runtime);
    check(live==before,"failed-code contexts, modules, suspended frames and queues release every allocation");
    failed_wrapper_cases();
    failed_unwind_cases();
}
