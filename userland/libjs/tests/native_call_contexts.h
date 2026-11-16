#ifndef OS64_JS_NATIVE_CALL_CONTEXT_CASES_H
#define OS64_JS_NATIVE_CALL_CONTEXT_CASES_H

/* Sequential host/guest fixtures: these identify context pointers independently
 * of function prototypes, callback arguments and the wrapper's turn owner. */
typedef struct {
    JSContext *contexts[3];
    os64_js_runtime_t *runtime;
    JSRuntime *other;
    unsigned calls;
} NativeContextFixture;
static NativeContextFixture *native_context_fixture;

static int native_context_number(JSContext *ctx)
{
    for (unsigned i = 0; i < 3; i++)
        if (native_context_fixture->contexts[i] == ctx) return (int)i + 1;
    return 0;
}

static JSValue native_context_probe(JSContext *ctx, bool data)
{
    JSNativeCallContexts got = {NULL, NULL};
    bool present = JS_GetNativeCallContexts(JS_GetRuntime(ctx), &got);
    check(present && native_context_number(got.incoming) && native_context_number(got.defining),
          "native invocation exposes both independently identified contexts");
    check(ctx == (data ? got.incoming : got.defining),
          "query preserves generic versus data callback context delivery");
    JSNativeCallContexts unrelated = got;
    check(!JS_GetNativeCallContexts(native_context_fixture->other, &unrelated) &&
          !unrelated.incoming && !unrelated.defining, "native call facts are isolated per runtime");
    native_context_fixture->calls++;
    return JS_NewInt32(ctx, 10 * native_context_number(got.incoming) + native_context_number(got.defining));
}

static JSValue native_context_generic(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self; (void)argc; (void)argv;
    return native_context_probe(ctx, false);
}

static JSValue native_context_data(JSContext *ctx, JSValueConst self, int argc,
                                    JSValueConst *argv, int magic, JSValue *data)
{
    (void)self; (void)magic; (void)data;
    JSValue result = native_context_probe(ctx, true);
    if (argc > 0 && JS_IsFunction(ctx, argv[0])) {
        JSNativeCallContexts before, after;
        check(JS_GetNativeCallContexts(JS_GetRuntime(ctx), &before), "outer native scope saved");
        JS_RunGC(JS_GetRuntime(ctx));
        JS_FreeValue(ctx, result);
        if (argc > 1 && JS_ToBool(ctx, argv[1])) os64_js_cancel(native_context_fixture->runtime);
        result = JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);
        check(JS_GetNativeCallContexts(JS_GetRuntime(ctx), &after) &&
              before.incoming == after.incoming && before.defining == after.defining,
              "native scope restored after nested normal or abrupt JS return");
    }
    return result;
}

static JSValue native_context_getter(JSContext *ctx, JSValueConst self)
{
    (void)self;
    return native_context_probe(ctx, false);
}

static JSValue native_context_setter(JSContext *ctx, JSValueConst self, JSValueConst value)
{
    (void)self;
    JSValue got = native_context_probe(ctx, false);
    int32_t actual = 0, expected = 0;
    check(JS_ToInt32(ctx, &actual, got) == 0 && JS_ToInt32(ctx, &expected, value) == 0 && actual == expected,
          "native setter retains incoming context before defining-context switch");
    JS_FreeValue(ctx, got);
    return JS_UNDEFINED;
}

static JSValue native_context_constructor(JSContext *ctx, JSValueConst target, int argc, JSValueConst *argv)
{
    (void)target; (void)argc; (void)argv;
    JSValue object = JS_NewObject(ctx);
    if (!JS_IsException(object)) JS_SetPropertyStr(ctx, object, "pair", native_context_probe(ctx, false));
    return object;
}

static void native_context_idle(JSRuntime *rt)
{
    JSNativeCallContexts got = {native_context_fixture->contexts[0], native_context_fixture->contexts[1]};
    check(!JS_GetNativeCallContexts(rt, &got) && !got.incoming && !got.defining,
          "native scope absent and output cleared after outer call");
}

static void native_call_context_cases(void)
{
    check(!JS_GetNativeCallContexts(NULL, NULL), "null native query refused");
    for (unsigned policy = 0; policy < 2; policy++) {
        os64_js_config_t config = fixture_config();
        os64_js_runtime_t *runtime = NULL;
        os64_js_realm_t *realms[3] = {NULL, NULL, NULL};
        os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config,
              policy ? OS64_JS_TEARDOWN_RECLAIM : OS64_JS_TEARDOWN_FATAL,
              OS64_JS_ABI_ID, &runtime, &out) == OS64_JS_OK, "native context runtime created");
        if (!runtime) continue;
        realms[0] = os64_js_primary_realm(runtime, OS64_JS_ABI_ID, &out);
        check(os64_js_realm_create(runtime, OS64_JS_ABI_ID, &realms[1], &out) == OS64_JS_OK &&
              os64_js_realm_create(runtime, OS64_JS_ABI_ID, &realms[2], &out) == OS64_JS_OK,
              "native context three-realm fixture created");
        if (!realms[1] || !realms[2]) { os64_js_destroy(runtime); continue; }
        NativeContextFixture fixture = {.runtime = runtime};
        fixture.other = JS_NewRuntime();
        check(fixture.other != NULL, "independent idle runtime created");
        native_context_fixture = &fixture;
        for (unsigned i = 0; i < 3; i++) fixture.contexts[i] = os64_js_realm_context(realms[i], OS64_JS_ABI_ID, &out);
        JSContext *a = fixture.contexts[0], *b = fixture.contexts[1], *c = fixture.contexts[2];
        JSRuntime *engine = JS_GetRuntime(a);
        native_context_idle(engine);
        native_context_idle(NULL);
        check(!JS_GetNativeCallContexts(engine, NULL), "null native output refused");
        for (unsigned i = 0; i < 3; i++) {
            JSContext *ctx = fixture.contexts[i];
            JSValue global = JS_GetGlobalObject(ctx);
            check(JS_SetPropertyStr(ctx, global, "probe", JS_NewCFunction(ctx, native_context_generic, "probe", 0)) == 1 &&
                  JS_SetPropertyStr(ctx, global, "dataProbe", JS_NewCFunctionData(ctx, native_context_data, 0, 0, 0, NULL)) == 1 &&
                  JS_SetPropertyStr(ctx, global, "neutralProbe", JS_NewCFunctionDataNeutral(ctx, native_context_data, 0, 0, 0, NULL)) == 1 &&
                  JS_SetPropertyStr(ctx, global, "Ctor", JS_NewCFunction2(ctx, native_context_constructor, "Ctor", 0, JS_CFUNC_constructor, 0)) == 1,
                  "generic data neutral and constructor fixtures installed");
            JSValue box = JS_NewObject(ctx);
            JSAtom value_key = JS_NewAtom(ctx, "value");
            JSCFunctionType getter = {.getter = native_context_getter};
            JSCFunctionType setter = {.setter = native_context_setter};
            check(JS_DefinePropertyGetSet(ctx, box, value_key,
                  JS_NewCFunction2(ctx, getter.generic, "get value", 0, JS_CFUNC_getter, 0),
                  JS_NewCFunction2(ctx, setter.generic, "set value", 1, JS_CFUNC_setter, 0),
                  JS_PROP_CONFIGURABLE) == 1, "native accessor fixture installed");
            JS_FreeAtom(ctx, value_key);
            check(JS_SetPropertyStr(ctx, global, "box", box) == 1, "accessor object published to fixture");
            JS_FreeValue(ctx, global);
        }
        const char *names[] = {"probe", "dataProbe", "neutralProbe", "Ctor", "box"};
        for (unsigned i = 0; i < sizeof(names)/sizeof(names[0]); i++) realm_share(c, a, names[i]);
        check(realm_execute(realms[2], "globalThis.foreignPromise=Promise", &out) == OS64_JS_OK,
              "foreign Promise constructor captured independently of callback");
        realm_share(c, a, "foreignPromise");
        check(realm_execute(realms[0],
              "if(probe()!==13||dataProbe()!==13||neutralProbe()!==13)throw 1;"
              "if(probe(...[])!==13||dataProbe(...[])!==13)throw 2;"
              "if(probe.call(null)!==33||probe.apply(null,[])!==33)throw 3;"
              "if(Reflect.apply(probe,null,[])!==13)throw 4;"
              "if(probe.bind(null)()!==13||new Proxy(probe,{})()!==13)throw 5;"
              "if(box.value!==13)throw 6;box.value=13;"
              "if(new Ctor().pair!==13||Reflect.construct(Ctor,[]).pair!==13)throw 7;"
              "try{Ctor();throw 8}catch(e){if(!(e instanceof TypeError)&&e.name!=='TypeError')throw e}"
              "if(eval('probe()')!==13||(0,eval)('probe()')!==13||Function('return probe()')()!==13)throw 9;"
              "if((function*(){yield probe()})().next().value!==13)throw 10;"
              "globalThis.pairJobs={};Promise.resolve().then(probe).then(x=>pairJobs.native=x);"
              "Promise.resolve().then(dataProbe).then(x=>pairJobs.data=x);"
              "foreignPromise.resolve().then(probe).then(x=>pairJobs.foreign=x);"
              "(async()=>{await 0;pairJobs.async=probe()})();", &out) == OS64_JS_OK,
              "native context facts cover raw call families eval resumes and jobs");
        check(realm_execute(realms[0],
              "if(pairJobs.native!==13||pairJobs.data!==13||pairJobs.foreign!==33||pairJobs.async!==13)throw 1",
              &out) == OS64_JS_OK, "job execution context is distinct from callback defining realm");
        native_context_idle(engine);
        JSValue raw_probe = realm_property(c, "probe");
        JSValue raw_result = JS_Call(a, raw_probe, JS_UNDEFINED, 0, NULL);
        int32_t raw_pair = 0;
        check(JS_ToInt32(a, &raw_pair, raw_result) == 0 && raw_pair == 13,
              "raw host invocation records incoming context without author frame");
        JS_FreeValue(a, raw_result); JS_FreeValue(c, raw_probe);
        raw_probe = realm_property(c, "dataProbe");
        raw_result = JS_Call(a, raw_probe, JS_UNDEFINED, 0, NULL);
        check(JS_ToInt32(a, &raw_pair, raw_result) == 0 && raw_pair == 13,
              "raw data callback records both contexts without author frame");
        JS_FreeValue(a, raw_result); JS_FreeValue(c, raw_probe);
        native_context_idle(engine);
        os64_js_module_t *module = NULL;
        const char *module_source = "if(probe()!==13)throw 1;await 0;if(dataProbe()!==13)throw 2;";
        check(os64_js_realm_module_compile(realms[0], OS64_JS_ABI_ID, module_source, os64_strlen(module_source),
              "native-context-module", NULL, &module, &out) == OS64_JS_OK &&
              os64_js_task_begin(runtime, OS64_JS_ABI_ID, "native-context-module", &out) == OS64_JS_OK &&
              os64_js_realm_module_run(realms[0], OS64_JS_ABI_ID, module, &out) == OS64_JS_OK &&
              os64_js_task_end(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK,
              "module execution and top-level await preserve native dispatcher facts");
        native_context_idle(engine);
        check(realm_execute(realms[1], "globalThis.nested=()=>probe();globalThis.abrupt=()=>{probe();throw 41}", &out) == OS64_JS_OK,
              "foreign JS reentry fixtures installed");
        realm_share(b, a, "nested"); realm_share(b, a, "abrupt");
        check(realm_execute(realms[0],
              "if(dataProbe(nested)!==22||neutralProbe(nested)!==22)throw 1;"
              "try{dataProbe(abrupt);throw 2}catch(e){if(e!==41)throw e}"
              "if(dataProbe(()=>dataProbe(nested))!==22)throw 3;", &out) == OS64_JS_OK,
              "nested data callbacks restore each enclosing scope on return and exception");
        native_context_idle(engine);
        JSValue pending = JS_NewObject(a);
        JS_Throw(a, JS_DupValue(a, pending));
        native_context_idle(engine);
        JSValue error = JS_GetException(a);
        check(JS_IsObject(error) && JS_VALUE_GET_PTR(error) == JS_VALUE_GET_PTR(pending),
              "native context query preserves pending exception");
        JS_FreeValue(a, error); JS_FreeValue(a, pending);
        JS_RunGC(engine);
        check(fixture.calls > 20, "native context fixture exercised actual callback bodies");
        check(realm_execute(realms[0], "dataProbe(()=>{for(;;){}},true)", &out) == OS64_JS_CANCELLED,
              "native reentry propagates runtime cancellation");
        native_context_idle(engine);
        if (fixture.other) JS_FreeRuntime(fixture.other);
        os64_js_destroy(runtime);
        native_context_fixture = NULL;
    }
}
#endif
