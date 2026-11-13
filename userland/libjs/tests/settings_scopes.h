#ifndef OS64_JS_SETTINGS_SCOPE_CASES_H
#define OS64_JS_SETTINGS_SCOPE_CASES_H

/* Native fixture contexts are identified independently of mutable prototypes. */
typedef struct {
    JSContext *contexts[3];
    JSRuntime *other;
    os64_js_runtime_t *runtime;
    unsigned depth, calls;
} SettingsFixture;
static SettingsFixture *settings_fixture;

static int settings_number(JSContext *ctx)
{
    for (unsigned i = 0; i < 3; i++)
        if (ctx == settings_fixture->contexts[i]) return (int)i + 1;
    return 0;
}

static int settings_pair(JSRuntime *rt)
{
    return 10 * settings_number(JS_GetEntryContext(rt)) +
           settings_number(JS_GetIncumbentContext(rt));
}

static JSValue settings_probe(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self; (void)argc; (void)argv;
    check(!JS_GetEntryContext(settings_fixture->other) &&
          !JS_GetIncumbentContext(settings_fixture->other), "settings are runtime local");
    JSNativeCallContexts native;
    check(JS_GetNativeCallContexts(JS_GetRuntime(ctx), &native) &&
          native.defining == settings_fixture->contexts[2] && ctx == native.defining,
          "native defining realm stays separate from entry and incumbent");
    settings_fixture->calls++;
    return JS_NewInt32(ctx, settings_pair(JS_GetRuntime(ctx)));
}

static JSValue settings_call(JSContext *entry, void *opaque)
{
    JSValueConst *function = opaque;
    return JS_Call(entry, *function, JS_UNDEFINED, 0, NULL);
}

static JSValue settings_callback(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    if (!argc) return JS_ThrowTypeError(ctx, "fixture callback required");
    JSRuntime *rt = JS_GetRuntime(ctx);
    int before = settings_pair(rt);
    JSValue function = JS_DupValue(ctx, argv[0]), result = JS_UNDEFINED;
    if (argc > 1 && JS_ToBool(ctx, argv[1])) os64_js_cancel(settings_fixture->runtime);
    check(JS_RunWithSettings(settings_fixture->contexts[1], settings_fixture->contexts[2],
                            settings_call, &function, &result), "callback scope admitted");
    check(settings_pair(rt) == before, "callback restores entry and incumbent after normal or abrupt call");
    JS_FreeValue(ctx, function);
    return result;
}

static JSValue settings_unexpected(JSContext *ctx, void *opaque)
{
    (void)ctx; (void)opaque;
    check(false, "refused scope must not enter body");
    return JS_UNDEFINED;
}

static JSValue settings_entry(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    if (!argc) return JS_ThrowTypeError(ctx, "fixture function required");
    JSRuntime *rt = JS_GetRuntime(ctx);
    int before = settings_pair(rt);
    JSValue function = JS_DupValue(ctx, argv[0]), result = JS_UNDEFINED;
    check(JS_RunWithSettings(settings_fixture->contexts[2], NULL, settings_call, &function, &result),
          "ordinary nested entry admitted without callback preparation");
    check(settings_pair(rt) == before, "ordinary entry restores callback backup and caller suppression");
    JS_FreeValue(ctx, function);
    return result;
}

static JSValue settings_depth(JSContext *ctx, void *opaque)
{
    (void)opaque;
    JSRuntime *rt = JS_GetRuntime(ctx);
    unsigned level = ++settings_fixture->depth;
    int before = settings_pair(rt);
    check(JS_GetEntryContext(rt) == ctx, "most recent host entry wins over outer entry");
    JSValue result = JS_NewInt32(ctx, 123);
    JSContext *next = settings_fixture->contexts[level % 3];
    if (level < JS_SETTINGS_SCOPE_LIMIT) {
        check(JS_RunWithSettings(next, next, settings_depth, NULL, &result), "bounded nested scope admitted");
    } else {
        JSValue pending = JS_NewObject(ctx);
        JS_Throw(ctx, JS_DupValue(ctx, pending));
        check(!JS_RunWithSettings(next, next, settings_unexpected, NULL, &result) &&
              JS_VALUE_GET_INT(result) == 123, "scope overflow refuses before output or body mutation");
        JSValue exception = JS_GetException(ctx);
        check(JS_IsObject(exception) && JS_VALUE_GET_PTR(exception) == JS_VALUE_GET_PTR(pending),
              "scope overflow preserves pending exception identity");
        JS_FreeValue(ctx, exception); JS_FreeValue(ctx, pending);
    }
    check(settings_pair(rt) == before, "nested callback skip counter and backup unwind together");
    settings_fixture->depth--;
    return result;
}

static JSValue settings_depth_native(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self; (void)argc; (void)argv;
    /* The enclosing entry scope is already one level deep. */
    settings_fixture->depth = 1;
    JSValue result = JS_UNDEFINED;
    int before = settings_pair(JS_GetRuntime(ctx));
    check(JS_RunWithSettings(settings_fixture->contexts[1], settings_fixture->contexts[2],
                            settings_depth, NULL, &result), "deep callbacks begin beneath author frame");
    check(settings_pair(JS_GetRuntime(ctx)) == before, "author incumbent returns after repeated suppression");
    settings_fixture->depth = 0;
    return result;
}

typedef struct {
    JSContext *context;
    const char *source;
    int flags;
    bool bytecode;
} SettingsEval;

static JSValue settings_eval(JSContext *entry, void *opaque)
{
    (void)entry;
    SettingsEval *eval = opaque;
    JSContext *ctx = eval->context;
    JSValue result = JS_Eval(ctx, eval->source, os64_strlen(eval->source), "settings-fixture.js",
                             eval->flags | (eval->bytecode ? JS_EVAL_FLAG_COMPILE_ONLY : 0));
    if (eval->bytecode && !JS_IsException(result)) {
        size_t size = 0;
        uint8_t *bytes = JS_WriteObject(ctx, &size, result, JS_WRITE_OBJ_BYTECODE);
        JS_FreeValue(ctx, result);
        if (!bytes) return JS_EXCEPTION;
        result = JS_ReadObject(ctx, bytes, size, JS_READ_OBJ_BYTECODE);
        js_free(ctx, bytes);
        if (!JS_IsException(result)) result = JS_EvalFunction(ctx, result);
    }
    return result;
}

static void settings_run(JSContext *entry, JSContext *incumbent, JSContext *code,
                         const char *source, int flags, bool bytecode, const char *label)
{
    SettingsEval eval = {code, source, flags, bytecode};
    JSValue result = JS_UNDEFINED;
    check(JS_RunWithSettings(entry, incumbent, settings_eval, &eval, &result), "script entry admitted");
    check(!JS_IsException(result), label);
    if (JS_IsException(result)) JS_FreeValue(code, JS_GetException(code));
    JS_FreeValue(code, result);
    check(!JS_GetEntryContext(JS_GetRuntime(entry)) && !JS_GetIncumbentContext(JS_GetRuntime(entry)),
          "settings absent after script leaves scope");
}

static JSValue settings_jobs(JSContext *entry, void *opaque)
{
    (void)opaque;
    JSRuntime *rt = JS_GetRuntime(entry);
    JSContext *job = NULL;
    unsigned count = 0;
    while (JS_IsJobPending(rt) && count++ < 32) {
        if (JS_ExecutePendingJob(rt, &job) < 0) return JS_EXCEPTION;
        check(settings_pair(rt) == 13, "suspended job activation leaves no stale incumbent");
    }
    check(!JS_IsJobPending(rt), "fixture jobs drain within bound");
    return JS_UNDEFINED;
}

static JSValue settings_snapshot(JSContext *entry, void *opaque)
{
    (void)opaque;
    return JS_NewInt32(entry, settings_pair(JS_GetRuntime(entry)));
}

static void settings_expect_call(JSContext *entry, JSContext *incumbent, JSValue function,
                                 int expected, const char *label)
{
    JSValue result = JS_UNDEFINED;
    int32_t number = 0;
    check(JS_RunWithSettings(entry, incumbent, settings_call, &function, &result), "raw callback entry admitted");
    check(JS_ToInt32(entry, &number, result) == 0 && number == expected, label);
    if (JS_IsException(result)) JS_FreeValue(entry, JS_GetException(entry));
    JS_FreeValue(entry, result); JS_FreeValue(entry, function);
}

static void settings_scope_cases(void)
{
    check(!JS_GetEntryContext(NULL) && !JS_GetIncumbentContext(NULL), "null runtime has no settings");
    for (unsigned policy = 0; policy < 2; policy++) {
        os64_js_config_t config = fixture_config();
        os64_js_runtime_t *runtime = NULL;
        os64_js_realm_t *realms[3] = {NULL, NULL, NULL};
        os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config,
              policy ? OS64_JS_TEARDOWN_RECLAIM : OS64_JS_TEARDOWN_FATAL,
              OS64_JS_ABI_ID, &runtime, &out) == OS64_JS_OK, "settings runtime created");
        if (!runtime) continue;
        realms[0] = os64_js_primary_realm(runtime, OS64_JS_ABI_ID, &out);
        check(os64_js_realm_create(runtime, OS64_JS_ABI_ID, &realms[1], &out) == OS64_JS_OK &&
              os64_js_realm_create(runtime, OS64_JS_ABI_ID, &realms[2], &out) == OS64_JS_OK,
              "settings fixture has three realms");
        if (!realms[1] || !realms[2]) { os64_js_destroy(runtime); continue; }
        SettingsFixture fixture = {.runtime = runtime};
        settings_fixture = &fixture;
        fixture.other = JS_NewRuntime();
        check(fixture.other != NULL, "settings independent runtime created");
        for (unsigned i = 0; i < 3; i++) fixture.contexts[i] = os64_js_realm_context(realms[i], OS64_JS_ABI_ID, &out);
        JSContext *a = fixture.contexts[0], *b = fixture.contexts[1], *c = fixture.contexts[2];
        JSRuntime *rt = JS_GetRuntime(a);
        for (unsigned i = 0; i < 3; i++) {
            JSContext *ctx = fixture.contexts[i];
            JSValue global = JS_GetGlobalObject(ctx);
            check(JS_SetPropertyStr(ctx, global, "settings", JS_NewCFunction(c, settings_probe, "settings", 0)) == 1 &&
                  JS_SetPropertyStr(ctx, global, "callback", JS_NewCFunction(a, settings_callback, "callback", 1)) == 1 &&
                  JS_SetPropertyStr(ctx, global, "enter", JS_NewCFunction(a, settings_entry, "enter", 1)) == 1 &&
                  JS_SetPropertyStr(ctx, global, "deep", JS_NewCFunction(a, settings_depth_native, "deep", 0)) == 1,
                  "settings probes with independent defining realms installed");
            JS_FreeValue(ctx, global);
        }
        JSValue sentinel = JS_NewInt32(a, 123);
        JSContext *other = fixture.other ? JS_NewContext(fixture.other) : NULL;
        check(other != NULL, "foreign-runtime context created");
        JSValue pending = JS_NewObject(a);
        JS_Throw(a, JS_DupValue(a, pending));
        check(!JS_RunWithSettings(NULL, c, settings_unexpected, NULL, &sentinel) &&
              !JS_RunWithSettings(a, c, NULL, NULL, &sentinel) &&
              !JS_RunWithSettings(a, c, settings_unexpected, NULL, NULL) &&
              (!other || !JS_RunWithSettings(a, other, settings_unexpected, NULL, &sentinel)) &&
              JS_VALUE_GET_INT(sentinel) == 123, "invalid scopes leave output untouched and never enter body");
        check(!JS_GetEntryContext(rt) && !JS_GetIncumbentContext(rt), "invalid scopes do not publish settings");
        JSValue error = JS_GetException(a);
        check(JS_IsObject(error) && JS_VALUE_GET_PTR(error) == JS_VALUE_GET_PTR(pending), "invalid scopes and getters preserve exception");
        JS_FreeValue(a, error); JS_FreeValue(a, pending);
        check(JS_RunWithSettings(a, NULL, settings_snapshot, NULL, &sentinel) && JS_VALUE_GET_INT(sentinel) == 10,
              "entry does not fabricate an incumbent without script or backup");
        check(JS_RunWithSettings(a, c, settings_snapshot, NULL, &sentinel) && JS_VALUE_GET_INT(sentinel) == 13,
              "callback backup supplies incumbent without author frame");

        /* Create with no author frame, then call both alone and beneath A.
         * Direct eval inherits this function's absence, not A's presence. */
        JSValue ctor = realm_property(b, "Function");
        JSValue text = JS_NewString(b, "return [settings(),eval('settings()'),()=>settings()]");
        JSValue raw = JS_CallConstructor(b, ctor, 1, &text);
        check(!JS_IsException(raw), "host-created dynamic function has no script provenance");
        JS_FreeValue(b, text); JS_FreeValue(b, ctor);
        JSValue global = JS_GetGlobalObject(a);
        check(JS_SetPropertyStr(a, global, "raw", JS_DupValue(a, raw)) == 1, "raw dynamic function shared");
        JS_FreeValue(a, global);
        check(realm_execute(realms[1],
              "globalThis.foreign=()=>settings();globalThis.fail=()=>{if(settings()!==22)throw 71;throw 42};"
              "globalThis.bound=settings.bind(null);globalThis.indirect=eval;globalThis.ForeignFunction=Function;"
              "globalThis.gen=(function*(){yield settings();try{yield settings()}finally{globalThis.finalPair=settings()}})();"
              "globalThis.advance=gen.next.bind(gen);globalThis.finish=gen.return.bind(gen);"
              "globalThis.throwGen=(function*(){try{yield 0}catch(e){return settings()}})();throwGen.next();"
              "globalThis.throwAdvance=throwGen.throw.bind(throwGen)", &out) == OS64_JS_OK,
              "script-bearing closures created outside opt-in scope");
        const char *names[] = {"foreign", "fail", "bound", "indirect", "ForeignFunction", "advance", "finish", "throwAdvance"};
        for (unsigned i = 0; i < sizeof(names)/sizeof(names[0]); i++) realm_share(b, a, names[i]);
        settings_run(a, c, a,
              "if(settings()!==11||settings.call(null)!==11||Reflect.apply(settings,null,[])!==11)throw 1;"
              "if(foreign()!==12||new Proxy(foreign,{})()!==12)throw 2;"
              "if(callback(bound)!==23||callback(foreign)!==22||settings()!==11)throw 3;"
              "if(callback(()=>callback(bound))!==23||callback(()=>settings())!==21)throw 4;"
              "if(enter(bound)!==31||callback(enter.bind(null,bound))!==33)throw 12;"
              "try{callback(fail);throw 5}catch(e){if(e!==42)throw e}if(settings()!==11)throw 6;"
              "if(eval('settings()')!==11||indirect('settings()')!==11)throw 7;"
              "if(ForeignFunction('return settings()')()!==12||indirect('(()=>settings())')()!==12)throw 8;"
              "let parts=raw();if(parts[0]!==11||parts[1]!==11)throw 9;globalThis.inherited=parts[2];"
              "if(advance().value!==12||advance().value!==12||!finish().done||throwAdvance(7).value!==12)throw 10;"
              "if(deep()!==123||settings()!==11)throw 11;",
              JS_EVAL_TYPE_GLOBAL, false, "three distinct roles survive calls callbacks eval generators and nested suppression");
        settings_run(a, c, b, "if(finalPair!==12)throw 1", JS_EVAL_TYPE_GLOBAL, false,
                     "generator return resumes script-bearing finally and restores frame");
        settings_expect_call(a, c, realm_property(a, "inherited"), 12,
                             "closure in null-provenance function inherits active outer script");
        settings_expect_call(a, c, realm_property(b, "bound"), 13, "bound native callback uses captured backup not function realm");
        settings_expect_call(a, c, realm_property(b, "foreign"), 12, "script closure overrides captured backup");
        JSValue result = JS_UNDEFINED;
        check(JS_RunWithSettings(a, c, settings_call, &raw, &result) && !JS_IsException(result), "raw dynamic callback executes");
        for (unsigned i = 0; i < 2; i++) {
            JSValue part = JS_GetPropertyUint32(a, result, i);
            int32_t number = 0;
            check(JS_ToInt32(a, &number, part) == 0 && number == 13,
                  "null-provenance dynamic function and direct eval leave callback backup selected");
            JS_FreeValue(a, part);
        }
        JSValue no_script = JS_GetPropertyUint32(a, result, 2);
        JS_FreeValue(a, result); JS_FreeValue(b, raw);
        settings_expect_call(a, c, no_script, 13, "closure created without active script retains absence");
        settings_run(a, c, b, "if(settings()!==12)throw 1;globalThis.cached=()=>settings()",
                     JS_EVAL_TYPE_GLOBAL, true, "serialized script root establishes provenance on instantiation");
        settings_expect_call(a, c, realm_property(b, "cached"), 12, "cached script closure retains provenance after return");
        settings_run(a, c, b,
              "if(settings()!==12)throw 1;globalThis.moduleFunction=()=>settings();"
              "await 0;if(settings()!==12)throw 2;globalThis.moduleDone=true;",
              JS_EVAL_TYPE_MODULE, true, "serialized module root and initial activation carry provenance");
        settings_run(a, c, b,
              "globalThis.asyncDone=false;(async()=>{await 0;if(settings()!==12)throw 1;asyncDone=true})();"
              "globalThis.nativeJob=0;Promise.resolve().then(bound).then(x=>nativeJob=x)",
              JS_EVAL_TYPE_GLOBAL, false, "async and native reaction fixtures queue without retaining dynamic scope");
        check(JS_RunWithSettings(a, c, settings_jobs, NULL, &result) && !JS_IsException(result), "jobs run in explicitly supplied host scope");
        if (JS_IsException(result)) JS_FreeValue(a, JS_GetException(a));
        JS_FreeValue(a, result);
        settings_run(a, c, b, "if(!moduleDone||!asyncDone||nativeJob!==13)throw 1",
                     JS_EVAL_TYPE_GLOBAL, false, "resumed module async and native job results use script presence or explicit backup");
        settings_expect_call(a, c, realm_property(b, "moduleFunction"), 12, "module closure retains script provenance");
        check(realm_execute(realms[0], "if(settings()!==0||foreign()!==0)throw 1", &out) == OS64_JS_OK,
              "unscoped production execution does not expose guessed settings");
        check(os64_js_task_begin(runtime, OS64_JS_ABI_ID, "settings-cancel", &out) == OS64_JS_OK,
              "cancellation fixture task opened");
        SettingsEval cancel = {a, "callback(()=>{for(;;){}},true)", JS_EVAL_TYPE_GLOBAL, false};
        check(JS_RunWithSettings(a, c, settings_eval, &cancel, &result) && JS_IsException(result),
              "cancellation propagates through nested callback settings scope");
        if (JS_IsException(result)) JS_FreeValue(a, JS_GetException(a));
        JS_FreeValue(a, result);
        check(!JS_GetEntryContext(rt) && !JS_GetIncumbentContext(rt), "cancelled scope leaves no entry or incumbent");
        check(os64_js_task_end(runtime, OS64_JS_ABI_ID, &out) != OS64_JS_OK, "settings do not clear outer cancellation latch");
        check(fixture.calls > 30, "settings fixture exercised native probes");
        if (other) JS_FreeContext(other);
        if (fixture.other) JS_FreeRuntime(fixture.other);
        os64_js_destroy(runtime);
        settings_fixture = NULL;
    }
}
#endif
