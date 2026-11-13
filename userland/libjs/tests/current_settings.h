#ifndef OS64_JS_CURRENT_SETTINGS_CASES_H
#define OS64_JS_CURRENT_SETTINGS_CASES_H

typedef struct {
    os64_js_runtime_t *runtime;
    os64_js_realm_t *realm[3];
    JSContext *ctx[3], *evaluate;
    JSAtom current, roles, prototype, then;
    unsigned prototype_realm, then_realm, iterator_calls;
    JSValue watch;
} CurrentFixture;
static CurrentFixture *current_fixture;
static JSClassID current_watch_slot, current_custom_slot;
static int current_number(JSContext *ctx)
{
    for (unsigned i = 0; i < 3; i++) if (ctx == current_fixture->ctx[i]) return (int)i + 1;
    return 0;
}
static void current_checked(JSContext *raw, os64_js_settings_role_t role)
{
    os64_js_outcome_t out;
    os64_js_realm_t *realm = os64_js_settings_realm(current_fixture->runtime, OS64_JS_ABI_ID, role, &out);
    int number = current_number(raw);
    check(number ? realm == current_fixture->realm[number - 1] && out.status == OS64_JS_OK
                 : realm == NULL && out.status == OS64_JS_ADMISSION_REFUSED,
          "checked current/native-caller query agrees with independent realm handles or refuses missing proof");
}
static int current_roles(JSRuntime *rt)
{
    JSContext *current = JS_GetCurrentContext(rt), *caller = JS_GetNativeCallerContext(rt);
    current_checked(current, OS64_JS_SETTINGS_CURRENT);
    current_checked(caller, OS64_JS_SETTINGS_NATIVE_CALLER);
    return 1000 * current_number(current) + 100 * current_number(JS_GetEntryContext(rt)) +
           10 * current_number(JS_GetIncumbentContext(rt)) + current_number(caller);
}
static JSValue current_get(JSContext *ctx, JSValueConst object, JSAtom atom, JSValueConst receiver)
{
    (void)object; (void)receiver;
    JSRuntime *rt = JS_GetRuntime(ctx);
    if (atom == current_fixture->current) {
        current_checked(JS_GetCurrentContext(rt), OS64_JS_SETTINGS_CURRENT);
        return JS_NewInt32(ctx, current_number(JS_GetCurrentContext(rt)));
    }
    if (atom == current_fixture->roles) return JS_NewInt32(ctx, current_roles(rt));
    if (atom == current_fixture->prototype) {
        current_fixture->prototype_realm = current_number(JS_GetCurrentContext(rt));
        return JS_NULL;
    }
    if (atom == current_fixture->then) {
        current_fixture->then_realm = current_number(JS_GetCurrentContext(rt));
        return JS_UNDEFINED;
    }
    return JS_UNDEFINED;
}
static JSValue current_probe(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self; (void)argc; (void)argv;
    JSNativeCallContexts facts;
    check(JS_GetNativeCallContexts(JS_GetRuntime(ctx), &facts) && facts.defining == ctx,
          "generic native argument still uses defining context");
    return JS_NewInt32(ctx, current_roles(JS_GetRuntime(ctx)));
}
static JSValue current_data(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic, JSValue *data)
{
    (void)self; (void)argc; (void)argv; (void)magic; (void)data;
    JSNativeCallContexts facts;
    check(JS_GetNativeCallContexts(JS_GetRuntime(ctx), &facts) && facts.incoming == ctx &&
          facts.defining == current_fixture->ctx[2], "data native argument preserves incoming independently of current C");
    return JS_NewInt32(ctx, current_roles(JS_GetRuntime(ctx)));
}
static JSValue current_reenter(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSContext *before = JS_GetCurrentContext(rt), *caller = JS_GetNativeCallerContext(rt);
    JSValue result;
    if (argc) {
        result = JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);
    } else {
        JSValue data = realm_property(current_fixture->ctx[2], "data");
        /* Deliberately choose an incoming argument different from current C. */
        result = JS_Call(current_fixture->ctx[0], data, JS_UNDEFINED, 0, NULL);
        JS_FreeValue(ctx, data);
    }
    check(JS_GetCurrentContext(rt) == before && JS_GetNativeCallerContext(rt) == caller,
          "normal and abrupt native reentry restore current and pre-entry snapshot");
    return result;
}
static JSValue current_scope_body(JSContext *ctx, void *opaque)
{
    (void)opaque;
    return JS_NewInt32(ctx, current_roles(JS_GetRuntime(ctx)));
}
static JSValue current_nested_scope(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self; (void)argc; (void)argv;
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSContext *before = JS_GetCurrentContext(rt), *caller = JS_GetNativeCallerContext(rt);
    os64_js_outcome_t out;
    JSValue result = JS_UNDEFINED;
    check(os64_js_realm_with_settings(current_fixture->realm[1], current_fixture->realm[2], OS64_JS_ABI_ID,
          current_scope_body, NULL, &result, &out) == OS64_JS_OK, "nested host preparation establishes a new realm context");
    check(JS_GetCurrentContext(rt) == before && JS_GetNativeCallerContext(rt) == caller,
          "nested host scope restores enclosing native execution and snapshot");
    return result;
}
static JSValue current_getter(JSContext *ctx, JSValueConst self)
{
    return current_probe(ctx, self, 0, NULL);
}
static JSValue current_custom(JSContext *ctx, JSValueConst function, JSValueConst self,
                              int argc, JSValueConst *argv, int flags)
{
    (void)function; (void)self; (void)argc; (void)argv; (void)flags;
    check(JS_GetCurrentContext(JS_GetRuntime(ctx)) == NULL, "unknown custom call masks inherited current realm");
    JSValue probe = realm_property(current_fixture->ctx[2], "probe");
    JSValue result = JS_Call(ctx, probe, JS_UNDEFINED, 0, NULL);
    JS_FreeValue(ctx, probe);
    return result;
}
static JSValue current_iterator(JSContext *ctx, JSValueConst self, int argc,
                                JSValueConst *argv, int *done, int magic)
{
    (void)self; (void)argc; (void)argv; (void)magic;
    JSNativeCallContexts facts;
    check(JS_GetNativeCallContexts(JS_GetRuntime(ctx), &facts) && facts.incoming == ctx &&
          facts.defining == current_fixture->ctx[2], "optimized iterator preserves incoming context delivery");
    *done = current_fixture->iterator_calls++ != 0;
    return JS_NewInt32(ctx, current_roles(JS_GetRuntime(ctx)));
}
static JSValue current_constructor(JSContext *ctx, JSValueConst target, int argc, JSValueConst *argv)
{
    (void)target; (void)argc; (void)argv;
    JSValue object = JS_NewObject(ctx);
    if (!JS_IsException(object)) JS_SetPropertyStr(ctx, object, "roles", JS_NewInt32(ctx, current_roles(JS_GetRuntime(ctx))));
    return object;
}
static JSValue current_eval(JSContext *entry, void *opaque)
{
    (void)entry;
    const char *source = opaque;
    return JS_Eval(current_fixture->evaluate, source, os64_strlen(source), "current-settings.js", JS_EVAL_TYPE_GLOBAL);
}
static void current_case(unsigned realm, const char *source, int expected, const char *label)
{
    os64_js_outcome_t out;
    JSValue result = JS_UNDEFINED;
    current_fixture->evaluate = current_fixture->ctx[realm];
    os64_js_status_t status = os64_js_realm_with_settings(current_fixture->realm[0], current_fixture->realm[0],
        OS64_JS_ABI_ID, current_eval, (void *)source, &result, &out);
    check(status == OS64_JS_OK && JS_IsNumber(result) && JS_VALUE_GET_INT(result) == expected, label);
    if (JS_IsException(result)) JS_FreeValue(current_fixture->ctx[0], JS_GetException(current_fixture->ctx[0]));
    JS_FreeValue(current_fixture->ctx[0], result);
    check(!JS_GetCurrentContext(JS_GetRuntime(current_fixture->ctx[0])) &&
          !JS_GetNativeCallerContext(JS_GetRuntime(current_fixture->ctx[0])), "current queries close when host scope exits");
}
static JSValue current_host(JSContext *ctx, void *opaque)
{
    (void)opaque;
    JSRuntime *rt = JS_GetRuntime(ctx);
    size_t bytes = os64_js_heap_bytes(current_fixture->runtime);
    JS_Throw(ctx, JS_NewInt32(ctx, 49));
    check(current_roles(rt) == 1110 && os64_js_heap_bytes(current_fixture->runtime) == bytes,
          "native host preparation establishes current without a caller or allocation");
    JSValue error = JS_GetException(ctx);
    check(JS_IsNumber(error) && JS_VALUE_GET_INT(error) == 49, "new settings queries preserve pending exception");
    JS_FreeValue(ctx, error);
    JSValue function = realm_property(current_fixture->ctx[2], "data");
    JSValue result = JS_Call(ctx, function, JS_UNDEFINED, 0, NULL);
    check(JS_IsNumber(result) && JS_VALUE_GET_INT(result) == 3111, "native-only data call enters C from prepared A");
    JS_FreeValue(ctx, result); JS_FreeValue(ctx, function);
    check(current_roles(rt) == 1110, "native-only call restores host execution context");
    return JS_UNDEFINED;
}
static JSValue current_raw_scope(JSContext *ctx, void *opaque)
{
    (void)opaque;
    os64_js_outcome_t out;
    check(JS_GetCurrentContext(JS_GetRuntime(ctx)) == ctx &&
          os64_js_settings_realm(current_fixture->runtime, OS64_JS_ABI_ID, OS64_JS_SETTINGS_CURRENT, &out) == NULL &&
          out.status == OS64_JS_ADMISSION_REFUSED, "raw-only preparation cannot publish checked current settings");
    return JS_UNDEFINED;
}
static JSValue current_jobs(JSContext *ctx, void *opaque)
{
    (void)opaque;
    JSContext *job = NULL;
    unsigned remaining = 64;
    while (JS_IsJobPending(JS_GetRuntime(ctx)) && remaining--) {
        check(JS_ExecutePendingJob(JS_GetRuntime(ctx), &job) >= 0, "current-settings resume job completes");
        check(JS_GetCurrentContext(JS_GetRuntime(ctx)) == ctx, "job return restores prepared host context");
    }
    check(!JS_IsJobPending(JS_GetRuntime(ctx)), "current-settings jobs finish within fixture bound");
    return JS_UNDEFINED;
}
static JSValue current_eval_function(JSContext *ctx, void *opaque)
{
    (void)ctx;
    return JS_EvalFunction(current_fixture->ctx[1], JS_DupValue(current_fixture->ctx[1], *(JSValue *)opaque));
}
static JSValue current_retire(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self; (void)argc; (void)argv;
    os64_js_outcome_t out;
    check(os64_js_realm_retire(current_fixture->realm[2], OS64_JS_ABI_ID, &out) == OS64_JS_OK &&
          !os64_js_settings_realm(current_fixture->runtime, OS64_JS_ABI_ID, OS64_JS_SETTINGS_CURRENT, &out) &&
          out.status == OS64_JS_REALM_RETIRED, "checked current query rechecks retired native defining realm");
    check(os64_js_realm_retire(current_fixture->realm[0], OS64_JS_ABI_ID, &out) == OS64_JS_OK &&
          !os64_js_settings_realm(current_fixture->runtime, OS64_JS_ABI_ID, OS64_JS_SETTINGS_NATIVE_CALLER, &out) &&
          out.status == OS64_JS_REALM_RETIRED, "checked native-caller query rechecks retired captured caller");
    check(JS_GetCurrentContext(JS_GetRuntime(ctx)) == current_fixture->ctx[2] &&
          JS_GetNativeCallerContext(JS_GetRuntime(ctx)) == current_fixture->ctx[0], "raw execution facts remain available during retirement unwind");
    os64_js_cancel(current_fixture->runtime);
    check(!os64_js_settings_realm(current_fixture->runtime, OS64_JS_ABI_ID, OS64_JS_SETTINGS_CURRENT, &out) &&
          out.status == OS64_JS_FAILED_RUNTIME, "checked current query refuses failed runtime");
    return JS_UNDEFINED;
}
static JSValue current_raw_call(JSContext *ctx, void *opaque)
{
    return JS_Call(ctx, *(JSValue *)opaque, JS_UNDEFINED, 0, NULL);
}
static void current_settings_cases(void)
{
    for (unsigned policy = 0; policy < 2; policy++) {
        os64_js_config_t config = fixture_config();
        os64_js_runtime_t *runtime = NULL;
        os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config, policy ? OS64_JS_TEARDOWN_RECLAIM : OS64_JS_TEARDOWN_FATAL,
              OS64_JS_ABI_ID, &runtime, &out) == OS64_JS_OK, "current-settings runtime created");
        if (!runtime) continue;
        CurrentFixture f = {.runtime = runtime}; current_fixture = &f;
        f.realm[0] = os64_js_primary_realm(runtime, OS64_JS_ABI_ID, &out);
        check(os64_js_realm_create(runtime, OS64_JS_ABI_ID, &f.realm[1], &out) == OS64_JS_OK &&
              os64_js_realm_create(runtime, OS64_JS_ABI_ID, &f.realm[2], &out) == OS64_JS_OK, "current-settings three realms created");
        if (!f.realm[1] || !f.realm[2]) { os64_js_destroy(runtime); current_fixture = NULL; continue; }
        for (unsigned i = 0; i < 3; i++) f.ctx[i] = os64_js_realm_context(f.realm[i], OS64_JS_ABI_ID, &out);
        JSContext *a = f.ctx[0], *b = f.ctx[1], *c = f.ctx[2];
        JSRuntime *engine = JS_GetRuntime(a);
        current_watch_slot = os64_js_class_id(&current_watch_slot);
        current_custom_slot = os64_js_class_id(&current_custom_slot);
        static JSClassExoticMethods exotic = {.get_property = current_get};
        const JSClassDef watch_definition = {.class_name = "CurrentWatch", .exotic = &exotic};
        const JSClassDef custom_definition = {.class_name = "CurrentCustom", .exotic = &exotic, .call = current_custom};
        check(JS_NewClass(engine, current_watch_slot, &watch_definition) == 0 &&
              JS_NewClass(engine, current_custom_slot, &custom_definition) == 0, "current-settings native observation classes registered");
        f.current = JS_NewAtom(a, "current"); f.roles = JS_NewAtom(a, "roles");
        f.prototype = JS_NewAtom(a, "prototype"); f.then = JS_NewAtom(a, "then");
        f.watch = JS_NewObjectClass(a, current_watch_slot);
        JSCFunctionType iterator_function = {.iterator_next = current_iterator};
        for (unsigned i = 0; i < 3; i++) {
            JSContext *ctx = f.ctx[i]; JSValue global = JS_GetGlobalObject(ctx);
            JS_SetPropertyStr(ctx, global, "watch", JS_DupValue(ctx, f.watch));
            JS_SetPropertyStr(ctx, global, "probe", JS_NewCFunction(c, current_probe, "probe", 0));
            JS_SetPropertyStr(ctx, global, "data", JS_NewCFunctionData(c, current_data, 0, 0, 0, NULL));
            JS_SetPropertyStr(ctx, global, "reenter", JS_NewCFunction(c, current_reenter, "reenter", 1));
            JS_SetPropertyStr(ctx, global, "nestedScope", JS_NewCFunction(c, current_nested_scope, "nestedScope", 0));
            JSCFunctionType getter_function = {.getter = current_getter};
            JSAtom getter_key = JS_NewAtom(ctx, "nativeValue");
            JS_DefinePropertyGetSet(ctx, global, getter_key,
                JS_NewCFunction2(c, getter_function.generic, "nativeValue", 0, JS_CFUNC_getter, 0), JS_UNDEFINED, JS_PROP_CONFIGURABLE);
            JS_FreeAtom(ctx, getter_key);
            JS_SetPropertyStr(ctx, global, "Native", JS_NewCFunction2(c, current_constructor, "Native", 0, JS_CFUNC_constructor, 0));
            JSValue custom = JS_NewObjectClass(ctx, current_custom_slot); JS_SetConstructorBit(ctx, custom, true);
            JS_SetPropertyStr(ctx, global, "custom", custom);
            JS_SetPropertyStr(ctx, global, "next", JS_NewCFunction2(c, iterator_function.generic, "next", 0, JS_CFUNC_iterator_next, 0));
            JS_FreeValue(ctx, global);
        }
        check(realm_execute(f.realm[1], "globalThis.foreign=()=>watch.current;globalThis.foreignRoles=()=>watch.roles;globalThis.abrupt=()=>{throw 42};"
              "globalThis.Build=function(){this.realm=watch.current};globalThis.trap=(target,self,args)=>Reflect.apply(target,self,args);"
              "globalThis.nativeCall=Function.prototype.call;"
              "globalThis.indirect=eval;globalThis.Dynamic=Function;globalThis.resolveB=null;"
              "new Promise(resolve=>resolveB=resolve);", &out) == OS64_JS_OK, "current-settings foreign fixtures created");
        const char *shared[] = {"foreign","foreignRoles","abrupt","Build","nativeCall","indirect","Dynamic","resolveB","trap"};
        for (unsigned i = 0; i < sizeof(shared)/sizeof(shared[0]); i++) realm_share(b, a, shared[i]);
        check(!JS_GetCurrentContext(NULL) && !JS_GetNativeCallerContext(NULL) && !JS_GetCurrentContext(engine),
              "new raw queries refuse absent runtime or host scope");
        check(os64_js_settings_realm(runtime, OS64_JS_ABI_ID, OS64_JS_SETTINGS_CURRENT, &out) == NULL &&
              out.status == OS64_JS_ADMISSION_REFUSED, "checked current query refuses idle setup");
        check(os64_js_task_begin(runtime, OS64_JS_ABI_ID, "current-settings", &out) == OS64_JS_OK, "current-settings task opened");
        JSValue result = JS_UNDEFINED;
        check(os64_js_realm_with_settings(f.realm[0], f.realm[0], OS64_JS_ABI_ID, current_host, NULL, &result, &out) == OS64_JS_OK,
              "prepared host current realm and raw native invocation pass"); JS_FreeValue(a, result);
        current_case(1, "watch.roles", 2120, "bytecode B distinguishes current B from host A");
        current_case(1, "probe()", 3122, "native C current differs from entry A and incumbent/caller B");
        current_case(1, "data()", 3122, "C-function-data current C is independent of incoming B");
        current_case(0, "reenter()", 3113, "native caller C is independent of chosen incoming A");
        current_case(0, "nestedScope()", 2231, "prepared B and captured incumbent C stay distinct from outer native caller A");
        current_case(0, "nativeValue", 3111, "native accessor uses defining current C and pre-entry A");
        current_case(0, "reenter(foreign)", 2, "foreign author reentry masks enclosing native C");
        current_case(0, "reenter(foreignRoles)", 2121, "native caller snapshot persists while reentrant script supplies current and incumbent B");
        current_case(0, "try{reenter(abrupt)}catch(e){if(e!==42)throw e}watch.current", 1, "abrupt reentry restores author A");
        current_case(0, "probe.bind(null)()", 3111, "bound wrapper does not invent a realm context");
        current_case(0, "new Proxy(probe,{})()", 3111, "empty proxy preserves caller until target invocation");
        current_case(0, "new Proxy(probe,{apply:trap})()", 3122, "foreign proxy trap establishes B before target C");
        current_case(0, "var revoked=Proxy.revocable(probe,{});revoked.revoke();try{revoked.proxy()}catch(e){}watch.current", 1,
                     "revoked proxy refusal restores author context");
        current_case(0, "nativeCall.call(probe,null)", 3112, "foreign call builtin B supplies native caller B");
        current_case(0, "Reflect.apply(data,null,[])", 3111, "Reflect.apply preserves its executing realm A");
        current_case(0, "indirect('watch.current')", 2, "indirect eval current exists without script provenance");
        current_case(0, "Dynamic('return watch.current')()", 2, "dynamic function current follows its code realm");
        current_case(1, "eval('watch.current')", 2, "direct eval keeps immediate execution realm B");
        current_case(0, "new Native().roles", 3111, "native constructor current and pre-entry realm are distinct");
        current_case(0, "try{Native()}catch(e){}watch.current", 1, "constructor-only refusal restores current");
        current_case(0, "Reflect.construct(Build,[],custom).realm", 2, "bytecode construction uses callee B");
        check(f.prototype_realm == 2, "constructor prototype lookup precedes body in callee B");
        current_case(0, "resolveB(watch);watch.current", 1, "Promise resolving builtin restores caller A");
        check(f.then_realm == 2, "Promise resolving builtin performs then lookup in its realm B");
        current_case(0, "custom()", 3110, "unknown custom call provides no inherited native-caller authority");
        current_case(0, "watch.current", 1, "custom call restoration returns to author A");
        current_case(1, "globalThis.generator=(function*(){yield watch.current;return watch.current})();generator.next().value", 2,
                     "generator initial resume enters B");
        realm_share(b, a, "generator");
        current_case(0, "generator.next().value", 2, "foreign generator resume uses code B");
        f.iterator_calls = 0;
        current_case(0, "[...{[Symbol.iterator](){return {next}}}][0]", 3111, "optimized iterator enters defining C with caller A");
        check(f.iterator_calls == 2, "optimized iterator body and completion both observed");
        current_case(1, "globalThis.asyncResult=0;(async()=>{asyncResult=watch.current;await 0;asyncResult=10*asyncResult+watch.current})();1", 1,
                     "async B enters before suspension");
        current_case(1, "globalThis.agResult=0;globalThis.ag=(async function*(){yield watch.current;return watch.current})();ag.next().then(x=>agResult=x.value);1", 1,
                     "async generator B starts under host A");
        check(os64_js_realm_with_settings(f.realm[0], f.realm[0], OS64_JS_ABI_ID, current_jobs, NULL, &result, &out) == OS64_JS_OK,
              "async current realm jobs execute under explicit host entry"); JS_FreeValue(a, result);
        current_case(1, "asyncResult*10+agResult", 222, "async and async generator resumes observe code realm B");
        const char *module_source = "globalThis.moduleCurrent=watch.current;await 0;moduleCurrent=10*moduleCurrent+watch.current";
        JSValue module = JS_Eval(b, module_source, os64_strlen(module_source), "current-module", JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        check(!JS_IsException(module) && os64_js_realm_with_settings(f.realm[0], f.realm[0], OS64_JS_ABI_ID,
              current_eval_function, &module, &result, &out) == OS64_JS_OK, "module B starts with host A current restored on return");
        JS_FreeValue(b, result); JS_FreeValue(b, module);
        check(os64_js_realm_with_settings(f.realm[0], f.realm[0], OS64_JS_ABI_ID, current_jobs, NULL, &result, &out) == OS64_JS_OK,
              "module top-level await resumes under explicit host scope"); JS_FreeValue(a, result);
        current_case(1, "moduleCurrent", 22, "module initial execution and resume use realm B");
        const char *cached_source = "watch.current";
        JSValue compiled = JS_Eval(b, cached_source, os64_strlen(cached_source), "current-cache", JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
        size_t length = 0; uint8_t *bytes = JS_WriteObject(b, &length, compiled, JS_WRITE_OBJ_BYTECODE);
        JSValue loaded = bytes ? JS_ReadObject(b, bytes, length, JS_READ_OBJ_BYTECODE) : JS_EXCEPTION;
        js_free(b, bytes); JS_FreeValue(b, compiled);
        check(!JS_IsException(loaded) && os64_js_realm_with_settings(f.realm[0], f.realm[0], OS64_JS_ABI_ID,
              current_eval_function, &loaded, &result, &out) == OS64_JS_OK && JS_IsNumber(result) && JS_VALUE_GET_INT(result) == 2,
              "cached script execution derives current realm B without serialized activation pointers");
        JS_FreeValue(b, result); JS_FreeValue(b, loaded);
        check(os64_js_task_end(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK, "current-settings task closed without budget reset");
        check(JS_RunWithSettings(a, NULL, current_raw_scope, NULL, &result), "raw-only scope entered"); JS_FreeValue(a, result);
        check(os64_js_task_begin(runtime, OS64_JS_ABI_ID, "current-retirement", &out) == OS64_JS_OK, "retirement fixture task opened");
        JSValue raw = JS_NewCFunction(c, current_retire, "retire", 0);
        check(os64_js_realm_with_settings(f.realm[0], f.realm[0], OS64_JS_ABI_ID, current_raw_call, &raw, &result, &out) == OS64_JS_CANCELLED,
              "retirement and cancellation unwind current/native-caller attribution");
        JS_FreeValue(c, raw); JS_FreeValue(a, result);
        check(!JS_GetCurrentContext(engine) && !JS_GetNativeCallerContext(engine) &&
              os64_js_task_end(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_CANCELLED, "failed task retains cancellation with no escaped current scope");
        JS_FreeValue(a, f.watch);
        JS_FreeAtom(a, f.current); JS_FreeAtom(a, f.roles); JS_FreeAtom(a, f.prototype); JS_FreeAtom(a, f.then);
        os64_js_destroy(runtime); current_fixture = NULL;
    }
}
#endif
