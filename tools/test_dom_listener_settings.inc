#include <os64/js_callbacks.h>

/* Four independently named roles: A captures, B owns the listener object,
 * C supplies native code, D owns the target and host task. */
static os64_js_runtime_t *listener_settings_runtime;
static os64_js_realm_t *listener_settings_realms[4];
static JSContext *listener_settings_contexts[4];
static os64_js_realm_t *listener_settings_entry, *listener_settings_incumbent;
static JSValue listener_settings_method;
static unsigned listener_settings_getters, listener_settings_calls;
static bool listener_settings_reenter;

static void listener_settings_check(JSContext *ctx, bool data)
{
    os64_js_outcome_t out;
    check(os64_js_settings_realm(listener_settings_runtime, OS64_JS_ABI_ID, OS64_JS_SETTINGS_ENTRY, &out)
          == listener_settings_entry, "listener settings: independent associated entry");
    check(os64_js_settings_realm(listener_settings_runtime, OS64_JS_ABI_ID, OS64_JS_SETTINGS_INCUMBENT, &out)
          == listener_settings_incumbent, "listener settings: conversion-time incumbent");
    check(os64_js_settings_realm(listener_settings_runtime, OS64_JS_ABI_ID, OS64_JS_SETTINGS_CURRENT, &out)
          == listener_settings_realms[2], "listener settings: native code executes in C");
    check(ctx == (data ? os64_js_realm_context(listener_settings_entry, OS64_JS_ABI_ID, &out)
                      : listener_settings_contexts[2]), "listener settings: native incoming convention");
}

static JSValue listener_settings_probe(JSContext *ctx, JSValueConst self, int argc,
    JSValueConst *argv, int magic, JSValue *data)
{
    (void)self; (void)argc; (void)argv; (void)magic; (void)data;
    listener_settings_check(ctx, true);
    listener_settings_calls++;
    return JS_UNDEFINED;
}

static JSValue listener_settings_get(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self; (void)argc; (void)argv;
    listener_settings_check(ctx, false);
    listener_settings_getters++;
    if (listener_settings_reenter) {
        listener_settings_reenter = false;
        const char *source = "D.dispatchEvent(new D.Event('once'))";
        JSValue result = JS_Eval(listener_settings_contexts[1], source, strlen(source), "nested-listener", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(result)) return result;
        JS_FreeValue(ctx, result);
        listener_settings_check(ctx, false);
    }
    return JS_DupValue(ctx, listener_settings_method);
}

static JSValue listener_settings_retire(JSContext *ctx, JSValueConst self, int argc,
    JSValueConst *argv, int magic)
{
    (void)self; (void)argc; (void)argv;
    os64_js_outcome_t out;
    if (os64_js_realm_retire(listener_settings_realms[magic], OS64_JS_ABI_ID, &out) != OS64_JS_OK)
        return JS_ThrowInternalError(ctx, "listener retirement failed");
    return JS_UNDEFINED;
}

static void listener_object_identity(JSContext *ctx)
{
    JSRuntime *engine = JS_GetRuntime(ctx);
    JSContext *creation = JS_NewContext(engine);
    check(creation != NULL, "object realm: raw context created");
    if (!creation) return;
    JSValue object = JS_NewObjectProto(creation, JS_NULL);
    check(JS_GetObjectRealm(ctx, object) == creation, "object realm: null-prototype identity");
    JSValue error = JS_NewObject(ctx);
    JS_Throw(ctx, JS_DupValue(ctx, error));
    check(JS_GetObjectRealm(ctx, object) == creation, "object realm: query with pending exception");
    JSValue pending = JS_GetException(ctx);
    check(JS_VALUE_GET_PTR(error) == JS_VALUE_GET_PTR(pending), "object realm: exception identity preserved");
    JS_FreeValue(ctx, error); JS_FreeValue(ctx, pending);
    JS_FreeContext(creation); JS_RunGC(engine);
    check(JS_GetObjectRealm(ctx, object) == NULL, "object realm: identity does not retain a collected context");
    for (int i = 0; i < 8; i++) {
        JSContext *replacement = JS_NewContext(engine);
        check(replacement && JS_GetObjectRealm(ctx, object) == NULL, "object realm: replacement context cannot acquire old identity");
        if (replacement) JS_FreeContext(replacement);
        JS_RunGC(engine);
    }
    JSValue prototype = JS_NewObject(ctx);
    check(JS_SetPrototype(ctx, object, prototype) == 1 && JS_GetObjectRealm(ctx, object) == NULL,
          "object realm: prototype replacement cannot invent a realm");
    JS_FreeValue(ctx, prototype); JS_FreeValue(ctx, object);
}

static void dom_listener_settings_cases(void)
{
    for (int policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; policy++)
    for (int mode = 0; mode < 12; mode++) {
        p2b_ms = 1000; p2b_us = 1000000;
        os64_js_config_t config = fixture_config(); config.limits.memory_bytes = 64u << 20;
        os64_js_runtime_t *runtime = NULL; os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config, policy, OS64_JS_ABI_ID, &runtime, &out) == OS64_JS_OK,
              "listener settings: runtime created");
        if (!runtime) continue;
        listener_settings_runtime = runtime;
        listener_settings_calls = listener_settings_getters = 0; listener_settings_reenter = false;
        listener_settings_method = JS_UNDEFINED;
        os64_dom_cluster_limits_t limits = {64u<<20, 16u<<20, 8u<<20, 256u<<20, 128u<<20};
        os64_dom_cluster_t *cluster = os64_dom_cluster_create_with_listener_settings(runtime, &limits, &out);
        check(cluster != NULL, "listener settings: opt-in cluster created");
        if (!cluster) { os64_js_destroy(runtime); continue; }
        os64_dom_t *bindings[4] = {0};
        listener_settings_realms[0] = os64_js_primary_realm(runtime, OS64_JS_ABI_ID, &out);
        bool ready = true;
        for (int i = 0; i < 4; i++) {
            if (i) check(os64_js_realm_create(runtime, OS64_JS_ABI_ID, &listener_settings_realms[i], &out) == OS64_JS_OK,
                         "listener settings: child created");
            listener_settings_contexts[i] = os64_js_realm_context(listener_settings_realms[i], OS64_JS_ABI_ID, &out);
            DomFixture f = fixture_configured(0, 0, &config); os64_js_destroy(f.runtime);
            os64_dom_options_t options = p2b_options("https://listeners.example/");
            os64_dom_admission_t a = os64_dom_create_in_realm(cluster, listener_settings_realms[i], f.document, f.state, &options, &out);
            bindings[i] = a.binding; ready &= a.binding != NULL;
            check(a.binding && a.consumed, "listener settings: binding installed");
            if (!a.consumed) { os64_page_state_free(f.state); os64_html_document_free(f.document); }
        }
        if (ready) {
            listener_settings_method = JS_NewCFunctionData(listener_settings_contexts[2], listener_settings_probe, 1, 0, 0, NULL);
            for (int i = 0; i < 4; i++) {
                JSContext *ctx = listener_settings_contexts[i]; JSValue global = JS_GetGlobalObject(ctx);
                for (int j = 0; j < 4; j++) {
                    char name[] = {(char)('A'+j),0};
                    check(JS_SetPropertyStr(ctx, global, name, JS_GetGlobalObject(listener_settings_contexts[j])) >= 0,
                          "listener settings: trusted realm exchange");
                    char retire[] = "retireA"; retire[6] += j;
                    check(JS_SetPropertyStr(ctx, global, retire, JS_NewCFunctionMagic(ctx, listener_settings_retire,
                          retire, 0, JS_CFUNC_generic_magic, j)) >= 0, "listener settings: retirement seam installed");
                }
                check(JS_SetPropertyStr(ctx, global, "probe", JS_DupValue(ctx, listener_settings_method)) >= 0,
                      "listener settings: native C function exported");
                JS_FreeValue(ctx, global);
                realm_script(listener_settings_realms[i], STORAGE_ASSERT "globalThis.errors=[];globalThis.seen=[];"
                    "addEventListener('error',e=>{errors.push(e.error);e.preventDefault()});", "listener settings: initial script");
            }
            JSContext *b = listener_settings_contexts[1]; JSValue global = JS_GetGlobalObject(b);
            JSValue object = JS_NewObjectProto(b, JS_NULL);
            JSAtom handle_event = JS_NewAtom(b, "handleEvent");
            check(JS_DefinePropertyGetSet(b, object, handle_event,
                  JS_NewCFunction(listener_settings_contexts[2], listener_settings_get, "get handleEvent", 0),
                  JS_UNDEFINED, JS_PROP_CONFIGURABLE) >= 0, "listener settings: foreign native getter on B object");
            JS_FreeAtom(b, handle_event);
            check(JS_SetPropertyStr(b, global, "listener", object) >= 0, "listener settings: B object exported");
            JS_FreeValue(b, global);
            listener_settings_entry = listener_settings_realms[1];
            listener_settings_incumbent = listener_settings_realms[0];
            if (mode == 0) {
                listener_object_identity(listener_settings_contexts[0]);
                realm_script(listener_settings_realms[0], "D.addEventListener('object',B.listener);"
                    "Object.setPrototypeOf(B.listener,A.Object.prototype);", "listener settings: capture object independently of prototype");
                realm_script(listener_settings_realms[2], "D.addEventListener('object',B.listener,{once:true});", "listener settings: duplicate preserves original incumbent and flags");
                os64_dom_event_t event = {.type="object"}; bool prevented = false;
                for (int i = 0; i < 2; i++)
                    check(os64_dom_dispatch(bindings[3], NULL, &event, &prevented, &out) == OS64_JS_OK,
                          "listener settings: host dispatch prepares before getter");
                check(listener_settings_getters == 2 && listener_settings_calls == 2, "listener settings: duplicate did not replace or become once");
                realm_script(listener_settings_realms[2], "D.dispatchEvent(new D.Event('object'));", "listener settings: script dispatch suppresses dispatcher's incumbent");
                listener_settings_entry = listener_settings_realms[2];
                realm_script(listener_settings_realms[0], "D.addEventListener('function',C.probe);", "listener settings: foreign function registered");
                event.type = "function";
                check(os64_dom_dispatch(bindings[3], NULL, &event, &prevented, &out) == OS64_JS_OK,
                      "listener settings: function callback enters C");
                listener_settings_entry = listener_settings_realms[1];
                realm_script(listener_settings_realms[0], "D.addEventListener('proxy',new B.Proxy(C.probe,{}));", "listener settings: callable proxy registered");
                event.type = "proxy";
                check(os64_dom_dispatch(bindings[3], NULL, &event, &prevented, &out) == OS64_JS_OK,
                      "listener settings: proxy entry differs from target code");
                listener_settings_entry = listener_settings_realms[2];
                realm_script(listener_settings_realms[0], "D.addEventListener('object-proxy',new C.Proxy(B.listener,{}));",
                             "listener settings: noncallable proxy registered");
                event.type = "object-proxy";
                check(os64_dom_dispatch(bindings[3], NULL, &event, &prevented, &out) == OS64_JS_OK,
                      "listener settings: object proxy creation entry differs from target object");
                listener_settings_entry = listener_settings_realms[1];
                realm_script(listener_settings_realms[1], "globalThis.sentinel={};globalThis.bad={get handleEvent(){throw sentinel}};"
                    "globalThis.missing={handleEvent:7};", "listener settings: getter failures created in B");
                realm_script(listener_settings_realms[0], "D.addEventListener('getter-throw',B.bad);D.addEventListener('missing-method',B.missing);",
                             "listener settings: getter failures registered in A");
                event.type = "getter-throw";
                check(os64_dom_dispatch(bindings[3], NULL, &event, &prevented, &out) == OS64_JS_EXCEPTION,
                      "listener settings: getter exception reported");
                realm_script(listener_settings_realms[0], "assert(D.errors[D.errors.length-1]===B.sentinel);",
                             "listener settings: getter exception identity survives reporting");
                event.type = "missing-method";
                check(os64_dom_dispatch(bindings[3], NULL, &event, &prevented, &out) == OS64_JS_EXCEPTION,
                      "listener settings: noncallable method reported");
                realm_script(listener_settings_realms[0], "assert(D.errors[D.errors.length-1] instanceof B.TypeError);",
                             "listener settings: missing-method TypeError belongs to associated entry");
                realm_script(listener_settings_realms[0], "globalThis.order=[];D.addEventListener({toString(){order.push('type');return 'ordering'}},B.listener,"
                    "{get capture(){order.push('capture');return false},get once(){order.push('once');return true}});"
                    "assert(order.join()==='type,capture,once');D.removeEventListener('ordering',B.listener);"
                    "D.addEventListener('once',B.listener,{once:true});", "listener settings: IDL conversion order and explicit removal");
                listener_settings_reenter = true; event.type = "once";
                unsigned before = listener_settings_calls;
                check(os64_dom_dispatch(bindings[3], NULL, &event, &prevented, &out) == OS64_JS_OK &&
                      listener_settings_calls == before+1, "listener settings: once removal precedes reentrant getter dispatch");
                check(os64_dom_dispatch(bindings[3], NULL, &event, &prevented, &out) == OS64_JS_OK &&
                      listener_settings_calls == before+1, "listener settings: once capture released");
                const char *raw = "globalThis.ran=false;try{D.addEventListener('raw',B.listener,{get capture(){ran=true}})}catch(e){};"
                    "if(ran)throw Error('capture after options');";
                JSValue value = JS_Eval(listener_settings_contexts[0], raw, strlen(raw), "unprepared-listener", JS_EVAL_TYPE_GLOBAL);
                check(!JS_IsException(value), "listener settings: missing capture scope refuses before options");
                if (JS_IsException(value)) JS_FreeValue(b, JS_GetException(b));
                JS_FreeValue(b, value);
                realm_script(listener_settings_realms[0], "let sentinel={};try{D.addEventListener('fail',B.listener,{get capture(){throw sentinel}});assert(false)}"
                    "catch(e){assert(e===sentinel)};Promise.resolve().then(()=>{try{D.addEventListener('job',B.listener);assert(false)}"
                    "catch(e){assert(e.name==='InvalidStateError')}});", "listener settings: conversion exception and unprepared job refusal");
                realm_script(listener_settings_realms[0], "globalThis.weak=(function(){let target=new D.EventTarget();"
                    "let listener=new B.Object();listener.handleEvent=()=>target;target.addEventListener('cycle',listener);"
                    "return new WeakRef(target)})();", "listener settings: engine-held target/callback cycle created");
                JS_RunGC(JS_GetRuntime(b));
                realm_script(listener_settings_realms[0], "assert(weak.deref()===undefined);", "listener settings: captured edge participates in cycle collection");
                for (unsigned cut = 1; cut <= 3; cut++) {
                    dom_fail_at = dom_attempts + cut;
                    const char *registration = "D.addEventListener('cut',B.listener)";
                    check(os64_js_realm_run(listener_settings_realms[0], registration, strlen(registration), "listener-allocation-cut", &out)
                          == OS64_JS_EXCEPTION, "listener settings: native registration allocation refusal");
                    dom_fail_at = 0;
                    os64_dom_script_ended(bindings[0], "listener-allocation-cut", &out);
                }
            } else if (mode <= 4) {
                realm_script(listener_settings_realms[1], "globalThis.scriptListener={get handleEvent(){seen.push('get');return C.probe}};", "listener settings: B getter returns C method");
                realm_script(listener_settings_realms[0], "D.addEventListener('retire',B.scriptListener);", "listener settings: four-role retirement registration");
                check(os64_js_realm_retire(listener_settings_realms[mode-1], OS64_JS_ABI_ID, &out) == OS64_JS_OK,
                      "listener settings: one role retired");
                os64_dom_event_t event = {.type="retire"}; bool prevented = false;
                os64_dom_dispatch(bindings[3], NULL, &event, &prevented, &out);
                check(listener_settings_calls == 0, "listener settings: retired role suppresses method");
                if (mode == 1 || mode == 2)
                    realm_script(listener_settings_realms[3], "assert(B.seen.length===0);", "listener settings: retired entry/incumbent suppresses getter too");
                if (mode == 3)
                    realm_script(listener_settings_realms[3], "assert(B.seen.join()==='get');", "listener settings: live getter allowed, retired returned code suppressed");
            } else if (mode == 5) {
                realm_script(listener_settings_realms[1], "globalThis.scriptListener={get handleEvent(){retireA();return C.probe}};", "listener settings: getter retires incumbent");
                realm_script(listener_settings_realms[0], "D.addEventListener('retire',B.scriptListener);", "listener settings: reentrant retirement registered");
                os64_dom_event_t event = {.type="retire"}; bool prevented = false;
                check(os64_dom_dispatch(bindings[3], NULL, &event, &prevented, &out) == OS64_JS_OK && listener_settings_calls == 0,
                      "listener settings: getter retirement rechecked before method");
            } else if (mode == 6) {
                realm_script(listener_settings_realms[0], "try{D.addEventListener('retire',B.listener,{get capture(){retireA();return false}})}"
                    "catch(e){if(e.name!=='InvalidStateError')throw e}", "listener settings: options retire captured incumbent");
                os64_dom_event_t event = {.type="retire"}; bool prevented = false;
                check(os64_dom_dispatch(bindings[3], NULL, &event, &prevented, &out) == OS64_JS_OK && listener_settings_getters == 0,
                      "listener settings: retired conversion never publishes listener");
            } else if (mode == 9) {
                realm_script(listener_settings_realms[2], "globalThis.node=document.createElement('div');", "listener settings: adoption target created");
                realm_script(listener_settings_realms[1], "globalThis.adopter={get handleEvent(){D.document.adoptNode(C.node);retireD();return C.probe}};",
                             "listener settings: getter adopts target into departing realm");
                realm_script(listener_settings_realms[0], "C.node.addEventListener('adopt',B.adopter);C.node.dispatchEvent(new Event('adopt'));",
                             "listener settings: adoption during listener lookup");
                check(listener_settings_calls == 0, "listener settings: getter adoption rechecks activity owner before method");
            } else if (mode >= 10) {
                realm_script(listener_settings_realms[1], mode == 10
                    ? "globalThis.slow={handleEvent(){globalThis.entered=1;while(true){}}};"
                    : "globalThis.slow={get handleEvent(){globalThis.entered=1;while(true){}}};",
                    "listener settings: script-dispatched interrupt fixture");
                realm_script(listener_settings_realms[0], "D.addEventListener('stop',B.slow);",
                             "listener settings: script-dispatched interrupt registered");
                const char *source = "try{D.dispatchEvent(new D.Event('stop'))}catch(e){globalThis.caught=new ArrayBuffer(8<<20)}";
                size_t before = large_asks;
                dom_clock_step = 10000;
                os64_js_realm_run(listener_settings_realms[0], source, strlen(source), "listener-interrupt", &out);
                dom_clock_step = 0;
                check(out.status == OS64_JS_LIMIT && large_asks == before,
                      "listener settings: interrupt cannot enter dispatch caller's catch");
                JSValue global = JS_GetGlobalObject(b), entered = JS_GetPropertyStr(b, global, "entered");
                int32_t ran = 0;
                check(JS_ToInt32(b, &ran, entered) == 0 && ran == 1,
                      "listener settings: interrupt reached getter or method");
                JS_FreeValue(b, entered); JS_FreeValue(b, global);
            } else {
                realm_script(listener_settings_realms[1], "globalThis.slow={handleEvent(){while(true){}}};", "listener settings: interruptible method created");
                realm_script(listener_settings_realms[0], "D.addEventListener('stop',B.slow);", "listener settings: interruptible listener registered");
                if (mode == 7) os64_js_cancel(runtime); else dom_clock_step = 200000;
                os64_dom_event_t event = {.type="stop"}; bool prevented = false;
                os64_dom_dispatch(bindings[3], NULL, &event, &prevented, &out);
                dom_clock_step = 0;
                check(out.status == OS64_JS_CANCELLED || out.status == OS64_JS_LIMIT,
                      "listener settings: failure remains terminal under nested settings");
            }
            check(JS_GetEntryContext(JS_GetRuntime(b)) == NULL && JS_GetIncumbentContext(JS_GetRuntime(b)) == NULL,
                  "listener settings: attribution restored after host return");
            for (int i = 0; i < 4; i++) memory_balanced(bindings[i], "listener settings before drain");
        }
        JS_FreeValueRT(JS_GetRuntime(listener_settings_contexts[0]), listener_settings_method);
        listener_settings_method = JS_UNDEFINED;
        os64_dom_cluster_drain(cluster); os64_js_destroy(runtime); os64_dom_cluster_free(cluster);
        check(live == 0 && dom_live == 0 && !dom_allocations, "listener settings: both teardown policies release records");
    }
}
