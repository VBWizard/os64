#ifndef OS64_JS_GLOBAL_THIS_CASES_H
#define OS64_JS_GLOBAL_THIS_CASES_H

/* The receiver is deliberately not a proxy: this tests the engine primitive
 * without supplying WindowProxy forwarding or browser security policy. */
static JSValue global_this_native(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)ctx; (void)self; (void)argc; (void)argv;
    return JS_UNDEFINED;
}

static void global_this_cases(void)
{
    /* Keep an exported receiver alive while its context is collected. The
     * context's hold must be released even though the receiver is not garbage. */
    JSRuntime *raw = JS_NewRuntime();
    JSContext *minimal = raw != NULL ? JS_NewContextRaw(raw) : NULL;
    check(minimal != NULL, "minimal embedding context created");
    if (minimal != NULL) {
        JSValue receiver = JS_NewObjectProto(minimal, JS_NULL);
        check(!JS_IsException(receiver) && JS_SetGlobalThis(minimal, receiver),
              "acyclic receiver retained by minimal context");
        JS_FreeContext(minimal);
        JS_RunGC(raw);
        JS_FreeValueRT(raw, receiver);
    }
    if (raw != NULL) JS_FreeRuntime(raw);
    for (unsigned policy = 0; policy < 2; policy++) {
        os64_js_config_t config = fixture_config();
        os64_js_runtime_t *runtime = NULL;
        os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config,
              policy ? OS64_JS_TEARDOWN_RECLAIM : OS64_JS_TEARDOWN_FATAL,
              OS64_JS_ABI_ID, &runtime, &out) == OS64_JS_OK,
              "global this runtime created");
        if (!runtime) continue;
        os64_js_realm_t *a = os64_js_primary_realm(runtime, OS64_JS_ABI_ID, &out), *b = NULL;
        check(os64_js_realm_create(runtime, OS64_JS_ABI_ID, &b, &out) == OS64_JS_OK,
              "global this child created");
        if (!b) { os64_js_destroy(runtime); continue; }
        JSContext *ac = os64_js_realm_context(a, OS64_JS_ABI_ID, &out);
        JSContext *bc = os64_js_realm_context(b, OS64_JS_ABI_ID, &out);
        JSContext *contexts[] = {ac, bc};
        os64_js_realm_t *realms[] = {a, b};
        for (unsigned i = 0; i < 2; i++) {
            JSContext *ctx = contexts[i];
            JSValue global = JS_GetGlobalObject(ctx);
            JSValue discarded = JS_NewObject(ctx);
            check(!JS_IsException(discarded) && JS_SetGlobalThis(ctx, discarded),
                  "initial embedding receiver accepted");
            JS_FreeValue(ctx, discarded);
            JSValue receiver = JS_NewObject(ctx);
            check(!JS_IsException(receiver) &&
                  JS_SetPropertyStr(ctx, receiver, "token", JS_NewInt32(ctx, 40 + (int)i)) == 1 &&
                  JS_SetPropertyStr(ctx, receiver, "cycle", JS_DupValue(ctx, receiver)) == 1 &&
                  JS_SetPropertyStr(ctx, receiver, "native", JS_NewCFunction(ctx, global_this_native, "native", 0)) == 1 &&
                  JS_SetGlobalThis(ctx, receiver) && JS_SetGlobalThis(ctx, receiver),
                  "receiver replacement and self-assignment retain ownership");
            check(JS_SetPropertyStr(ctx, global, "expected", JS_DupValue(ctx, receiver)) == 1,
                  "receiver oracle installed on real global");
            JSValue error = JS_NewObject(ctx);
            JS_Throw(ctx, JS_DupValue(ctx, error));
            check(!JS_SetGlobalThis(ctx, JS_NULL) && !JS_SetGlobalThis(ctx, JS_UNDEFINED) &&
                  !JS_SetGlobalThis(ctx, JS_NewInt32(ctx, 1)),
                  "non-object receivers refused");
            JSValue pending = JS_GetException(ctx);
            check(JS_IsObject(pending) && JS_VALUE_GET_PTR(pending) == JS_VALUE_GET_PTR(error),
                  "receiver refusal preserves pending exception");
            JS_FreeValue(ctx, pending);
            JS_FreeValue(ctx, error);
            JS_FreeValue(ctx, receiver);
            JS_FreeValue(ctx, global);
            check(realm_execute(realms[i],
                  "if(this!==expected||this===globalThis)throw 1;"
                  "var stored=7;let lexical=9;function sloppy(){return this}"
                  "function strict(){'use strict';return this}"
                  "if(globalThis.stored!==7||'stored' in expected||'lexical' in globalThis)throw 2;"
                  "if(sloppy()!==expected||sloppy.call(null)!==expected||"
                  "sloppy.call(undefined)!==expected||strict()!==undefined||strict.call(null)!==null)throw 3;"
                  "if(sloppy.call(12).valueOf()!==12)throw 4;"
                  "var explicit={};if(sloppy.call(explicit)!==explicit||strict.call(explicit)!==explicit)throw 5;"
                  "if(eval('this')!==expected||(0,eval)('this')!==expected||(()=>this)()!==expected)throw 6;"
                  "if(Function('return this')()!==expected||Function('\"use strict\";return this')()!==undefined)throw 7;"
                  "if((function(){return eval('this')}).call(explicit)!==explicit)throw 8;"
                  "if((function*(){yield this})().next().value!==expected)throw 10;"
                  "(async function(){return this})().then(x=>{if(x!==expected)throw 11});"
                  "(async function*(){yield this})().next().then(x=>{if(x.value!==expected)throw 12});"
                  "globalThis.strictScript=(function(){'use strict';return ()=>this})();"
                  "if(strictScript()!==undefined)throw 9;", &out) == OS64_JS_OK,
                  "global receiver selection preserves storage strictness and explicit this");
            check(realm_execute(realms[i], "'use strict';if(this!==expected)throw 1", &out) == OS64_JS_OK,
                  "strict top-level script keeps embedding receiver");
            const char *source = "if(this!==expected||(0,eval)('this')!==expected)throw 1;var cachedThis=this;";
            os64_js_bytecode_t written = {.keep = true};
            check(os64_js_realm_run_bytecode(realms[i], source, os64_strlen(source),
                  "global-this-cache", &written, &out) == OS64_JS_OK && written.kept != NULL,
                  "compiled script uses embedding receiver");
            os64_js_bytecode_t read = {.blob = written.kept, .blob_length = written.kept_length};
            check(os64_js_realm_run_bytecode(realms[i], source, os64_strlen(source),
                  "global-this-cache", &read, &out) == OS64_JS_OK && read.from_blob,
                  "cached script uses embedding receiver");
            os64_free(written.kept);
            os64_js_module_t *module = NULL;
            source = "if(this!==undefined)throw 1;await Promise.resolve();if(this!==undefined)throw 2;";
            check(os64_js_realm_module_compile(realms[i], OS64_JS_ABI_ID, source, os64_strlen(source),
                  "global-this-module", NULL, &module, &out) == OS64_JS_OK &&
                  os64_js_task_begin(runtime, OS64_JS_ABI_ID, "global-this-module", &out) == OS64_JS_OK &&
                  os64_js_realm_module_run(realms[i], OS64_JS_ABI_ID, module, &out) == OS64_JS_OK,
                  "module with undefined this starts");
            check(os64_js_task_end(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK &&
                  os64_js_realm_module_state(realms[i], module, &out) == OS64_JS_MODULE_FULFILLED,
                  "resumed module this remains undefined");
        }
        realm_share(bc, ac, "sloppy");
        realm_share(bc, ac, "strict");
        check(realm_execute(a,
              "if(sloppy().token!==41||sloppy.call(null).token!==41||strict()!==undefined)throw 1;"
              "var foreignBound=sloppy.bind(null);if(foreignBound().token!==41)throw 2;"
              "if(new Proxy(sloppy,{})().token!==41)throw 3;"
              "Promise.resolve().then(sloppy).then(x=>{if(x.token!==41)throw 4});", &out) == OS64_JS_OK,
              "foreign functions bound calls proxies and Promise jobs select their code realm");
        check(realm_execute(a, "globalThis={poison:true};if(this.token!==40||(0,eval)('this').token!==40)throw 1", &out)
              == OS64_JS_OK, "globalThis property mutation cannot change internal receiver");
        check(realm_execute(b, "delete globalThis.expected;delete globalThis.cachedThis", &out) == OS64_JS_OK,
              "drop explicit child receiver roots");
        JS_RunGC(JS_GetRuntime(bc));
        check(realm_execute(b, "if(this.token!==41||this.cycle!==this)throw 1", &out) == OS64_JS_OK,
              "context retains cyclic receiver through GC");
        JSValue explicit = JS_NewObject(ac);
        JSValue value = JS_EvalThis(ac, explicit, "this", 4, "explicit-this", JS_EVAL_TYPE_GLOBAL);
        check(JS_IsObject(value) && JS_VALUE_GET_PTR(value) == JS_VALUE_GET_PTR(explicit),
              "JS_EvalThis preserves explicit embedding receiver");
        JS_FreeValue(ac, value);
        JS_FreeValue(ac, explicit);
        os64_js_teardown_report_t report;
        os64_js_destroy_report(runtime, &report);
        check(!report.leaked, "embedding receiver graph tears down without reclaimed leaks");
    }
}

#endif
