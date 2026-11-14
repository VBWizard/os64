#include <os64/js_callbacks.h>
/* Native opt-in consumer tests. A registers, B owns the timer queue, C owns
 * the callable. Proxy creation in B makes associated entry independent too. */
static os64_js_runtime_t *timer_settings_runtime;
static os64_js_realm_t *timer_settings_realms[3];
static os64_js_realm_t *timer_settings_entry, *timer_settings_incumbent;
static unsigned timer_settings_calls;
static JSValue timer_settings_probe(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self; (void)argc; (void)argv;
    os64_js_outcome_t out;
    check(os64_js_settings_realm(timer_settings_runtime, OS64_JS_ABI_ID, OS64_JS_SETTINGS_ENTRY, &out)
          == timer_settings_entry, "timer settings: independently expected entry");
    check(os64_js_settings_realm(timer_settings_runtime, OS64_JS_ABI_ID, OS64_JS_SETTINGS_INCUMBENT, &out)
          == timer_settings_incumbent, "timer settings: independently expected incumbent");
    check(os64_js_settings_realm(timer_settings_runtime, OS64_JS_ABI_ID, OS64_JS_SETTINGS_CURRENT, &out)
          == timer_settings_realms[2], "timer settings: native callable executes in C");
    check(ctx == os64_js_realm_context(timer_settings_realms[2], OS64_JS_ABI_ID, &out),
          "timer settings: native argument convention preserved");
    timer_settings_calls++;
    return JS_UNDEFINED;
}

static JSValue timer_settings_data_probe(JSContext *ctx, JSValueConst self, int argc,
    JSValueConst *argv, int magic, JSValue *data)
{
    (void)self; (void)argc; (void)argv; (void)magic; (void)data;
    os64_js_outcome_t out;
    check(ctx == os64_js_realm_context(timer_settings_entry, OS64_JS_ABI_ID, &out),
          "timer settings: data callback incoming argument follows entry, not report owner");
    check(os64_js_settings_realm(timer_settings_runtime, OS64_JS_ABI_ID, OS64_JS_SETTINGS_INCUMBENT, &out)
          == timer_settings_incumbent, "timer settings: data callback captured incumbent");
    check(os64_js_settings_realm(timer_settings_runtime, OS64_JS_ABI_ID, OS64_JS_SETTINGS_CURRENT, &out)
          == timer_settings_realms[2], "timer settings: data callback current is defining C");
    timer_settings_calls++;
    return JS_UNDEFINED;
}

static unsigned timer_proxy_finalized;
static void timer_proxy_finalizer(JSRuntime *runtime, JSValue value)
{
    (void)runtime; (void)value;
    timer_proxy_finalized++;
}

static void timer_proxy_lifetime(JSContext *owner)
{
    JSRuntime *engine = JS_GetRuntime(owner);
    JSClassID id = 0; JS_NewClassID(&id);
    JSClassDef definition = {.class_name="TimerProxyLifetime", .finalizer=timer_proxy_finalizer};
    check(JS_NewClass(engine, id, &definition) == 0, "timer proxy: lifetime class installed");
    JSContext *creation = JS_NewContext(engine);
    check(creation != NULL, "timer proxy: separate creation context");
    if (!creation) return;
    timer_proxy_finalized = 0;
    JSValue global = JS_GetGlobalObject(creation);
    check(JS_SetPropertyStr(creation, global, "lifetime", JS_NewObjectClass(creation, id)) >= 0 &&
          JS_SetPropertyStr(creation, global, "target", JS_NewCFunction(owner, timer_settings_probe, "target", 0)) >= 0,
          "timer proxy: context lifetime sentinel and foreign target installed");
    JS_FreeValue(creation, global);
    const char *source = "var pair=Proxy.revocable(target,{});pair.revoke();pair.proxy";
    JSValue proxy = JS_Eval(creation, source, strlen(source), "proxy-lifetime", JS_EVAL_TYPE_GLOBAL);
    check(!JS_IsException(proxy), "timer proxy: revoked proxy created");
    JSValue sentinel = JS_NewObject(owner);
    JS_Throw(owner, JS_DupValue(owner, sentinel));
    check(JS_GetCallbackRealm(owner, proxy) == creation, "timer proxy: revoked associated realm remains creation context");
    JSValue exception = JS_GetException(owner);
    check(JS_VALUE_GET_PTR(exception) == JS_VALUE_GET_PTR(sentinel), "timer proxy: query preserves pending exception identity");
    JS_FreeValue(owner, exception); JS_FreeValue(owner, sentinel);
    JS_FreeContext(creation);
    JS_RunGC(engine);
    check(timer_proxy_finalized == 0 && JS_GetCallbackRealm(owner, proxy) == creation,
          "timer proxy: external callback holds creation context across GC");
    JS_FreeValue(owner, proxy);
    JS_RunGC(engine);
    check(timer_proxy_finalized == 1, "timer proxy: context-proxy cycle collects after last external callback");
    check(JS_GetCallbackRealm(owner, JS_UNDEFINED) == NULL, "timer proxy: unsupported value has no guessed realm");
}

static void dom_timer_settings_cases(void)
{
    for (int policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; policy++)
    for (int retirement = -1; retirement < 6; retirement++) {
        p2b_ms = 1000; p2b_us = 1000000;
        os64_js_config_t config = fixture_config(); config.limits.memory_bytes = 64u << 20;
        os64_js_outcome_t out;
        os64_js_runtime_t *runtime = NULL;
        check(os64_js_create_with_teardown(&config, policy, OS64_JS_ABI_ID, &runtime, &out) == OS64_JS_OK,
              "timer settings: runtime created");
        if (!runtime) continue;
        timer_settings_runtime = runtime; timer_settings_calls = 0;
        os64_dom_cluster_limits_t limits = {64u<<20, 16u<<20, 8u<<20, 256u<<20, 128u<<20};
        os64_dom_cluster_t *cluster = os64_dom_cluster_create_with_timer_settings(runtime, &limits, &out);
        check(cluster != NULL, "timer settings: opt-in cluster created");
        if (!cluster) { os64_js_destroy(runtime); continue; }
        os64_dom_t *bindings[3] = {0}; JSContext *contexts[3] = {0};
        timer_settings_realms[0] = os64_js_primary_realm(runtime, OS64_JS_ABI_ID, &out);
        for (int i = 0; i < 3; i++) {
            if (i) check(os64_js_realm_create(runtime, OS64_JS_ABI_ID, &timer_settings_realms[i], &out) == OS64_JS_OK,
                         "timer settings: child created");
            contexts[i] = os64_js_realm_context(timer_settings_realms[i], OS64_JS_ABI_ID, &out);
            retirement_realms[i] = timer_settings_realms[i];
            DomFixture f = fixture_configured(0, 0, &config); os64_js_destroy(f.runtime);
            os64_dom_options_t options = p2b_options("https://settings.example/");
            os64_dom_admission_t a = os64_dom_create_in_realm(cluster, timer_settings_realms[i], f.document, f.state, &options, &out);
            bindings[i] = a.binding;
            check(a.binding && a.consumed, "timer settings: binding installed");
            if (!a.consumed) { os64_page_state_free(f.state); os64_html_document_free(f.document); }
        }
        if (bindings[0] && bindings[1] && bindings[2]) {
            for (int i = 0; i < 3; i++) {
                JSValue global = JS_GetGlobalObject(contexts[i]);
                for (int j = 0; j < 3; j++) {
                    char name[] = {(char)('A'+j),0};
                    check(JS_SetPropertyStr(contexts[i], global, name, JS_GetGlobalObject(contexts[j])) >= 0,
                          "timer settings: trusted realm exchange");
                }
                check(JS_SetPropertyStr(contexts[i], global, "probe", JS_NewCFunction(contexts[2], timer_settings_probe, "probe", 0)) >= 0,
                      "timer settings: C native callback installed");
                check(JS_SetPropertyStr(contexts[i], global, "dataProbe", JS_NewCFunctionData(contexts[2], timer_settings_data_probe, 0, 0, 0, NULL)) >= 0,
                      "timer settings: C data callback installed");
                check(JS_SetPropertyStr(contexts[i], global, "retireA", JS_NewCFunctionMagic(contexts[i], retirement_mark,
                      "retireA", 0, JS_CFUNC_generic_magic, 0)) >= 0, "timer settings: retirement seam");
                JS_FreeValue(contexts[i], global);
                realm_script(timer_settings_realms[i], STORAGE_ASSERT "globalThis.seen=[];globalThis.errors=[];globalThis.errorRealms=[];"
                    "addEventListener('error',e=>{errors.push(e.message);errorRealms.push(e.error instanceof B.TypeError);e.preventDefault()});", "timer settings: bootstrap");
            }
            check(os64_js_enable_host_settings(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_BUSY,
                  "timer settings: late configuration refused");
            timer_settings_entry = timer_settings_realms[2];
            timer_settings_incumbent = timer_settings_realms[0];
            if (retirement < 0) {
                timer_proxy_lifetime(contexts[0]);
                timer_settings_entry = timer_settings_realms[0];
                os64_js_bytecode_t bytecode = {.keep=true};
                check(os64_js_realm_run_bytecode(timer_settings_realms[0], "probe()", 7, "settings-bytecode", &bytecode, &out) == OS64_JS_OK && bytecode.kept,
                      "timer settings: compiled classic root prepares settings");
                os64_js_bytecode_t cached = {.blob=bytecode.kept, .blob_length=bytecode.kept_length};
                check(os64_js_realm_run_bytecode(timer_settings_realms[0], "probe()", 7, "settings-bytecode", &cached, &out) == OS64_JS_OK && cached.from_blob,
                      "timer settings: cached classic root prepares settings");
                os64_free(bytecode.kept);
                os64_js_module_t *module = NULL;
                check(os64_js_realm_task_begin(timer_settings_realms[0], OS64_JS_ABI_ID, "settings-module", &out) == OS64_JS_OK &&
                      os64_js_realm_module_compile(timer_settings_realms[0], OS64_JS_ABI_ID, "probe()", 7, "settings-module",
                          "https://settings.example/module.js", &module, &out) == OS64_JS_OK &&
                      os64_js_realm_module_run(timer_settings_realms[0], OS64_JS_ABI_ID, module, &out) == OS64_JS_OK,
                      "timer settings: module root prepares settings");
                check(os64_js_task_end(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK &&
                      os64_js_realm_module_release(timer_settings_realms[0], OS64_JS_ABI_ID, module, &out) == OS64_JS_OK,
                      "timer settings: module task closes");
                check(timer_settings_calls == 3, "timer settings: root bodies executed");
                timer_settings_calls = 0;
                timer_settings_entry = timer_settings_realms[2];
                realm_script(timer_settings_realms[0], "B.setTimeout(C.probe,0);", "timer settings: foreign native registered");
                check(os64_dom_timer_fire(bindings[1], p2b_ms, &out) && out.status == OS64_JS_OK && timer_settings_calls == 1,
                      "timer settings: foreign native delivered");
                realm_script(timer_settings_realms[0], "B.setTimeout(C.probe.bind(null),0);", "timer settings: bound native registered");
                check(os64_dom_timer_fire(bindings[1], p2b_ms, &out) && out.status == OS64_JS_OK && timer_settings_calls == 2,
                      "timer settings: bound native delivered");
                timer_settings_entry = timer_settings_realms[1];
                realm_script(timer_settings_realms[0], "globalThis.proxy=new B.Proxy(C.probe,{});"
                    "Object.setPrototypeOf(proxy,A.Function.prototype);B.setTimeout(proxy,0);"
                    "B.setTimeout(Function.prototype.bind.call(proxy,null),0);", "timer settings: proxy entry differs from target");
                for (int i = 0; i < 2; i++)
                    check(os64_dom_timer_fire(bindings[1], p2b_ms, &out) && out.status == OS64_JS_OK,
                          "timer settings: proxy and bound proxy delivered");
                check(timer_settings_calls == 4, "timer settings: prototype spoof did not change associated entry");
                realm_script(timer_settings_realms[0], "B.setTimeout(new B.Proxy(C.dataProbe,{}),0);",
                             "timer settings: proxy around native data function");
                check(os64_dom_timer_fire(bindings[1], p2b_ms, &out) && out.status == OS64_JS_OK && timer_settings_calls == 5,
                      "timer settings: data callback separates incoming, defining and reporting contexts");
                timer_settings_entry = timer_settings_realms[2];
                timer_settings_incumbent = timer_settings_realms[2];
                realm_script(timer_settings_realms[2], "globalThis.callback=function(x){assert(this===B&&x===42);"
                    "B.clearInterval(A.interval);probe();B.seen.push('self');B.setTimeout(()=>B.seen.push('nested'),0)};",
                    "timer settings: author callback created");
                realm_script(timer_settings_realms[0], "globalThis.interval=B.setInterval(C.callback,0,42);", "timer settings: self-removing interval registered");
                check(os64_dom_timer_fire(bindings[1], p2b_ms, &out) && out.status == OS64_JS_OK,
                      "timer settings: self-removal retains executing callback and arguments");
                check(os64_dom_timer_fire(bindings[1], p2b_ms, &out) && out.status == OS64_JS_OK,
                      "timer settings: nested timer fires after self-removal");
                realm_script(timer_settings_realms[0], "assert(B.seen.join()==='self,nested');"
                    "B.clearTimeout(B.setTimeout(C.probe,0));B.setTimeout(\"seen.push('string')\",0);",
                    "timer settings: cancellation and string handler");
                check(os64_dom_timer_fire(bindings[1], p2b_ms, &out) && out.status == OS64_JS_OK,
                      "timer settings: string handler uses queue realm");
                realm_script(timer_settings_realms[0], "assert(B.seen.join()==='self,nested,string');"
                    "let sentinel={};try{B.setTimeout(C.probe,{valueOf(){throw sentinel}});assert(false)}catch(e){assert(e===sentinel)};",
                    "timer settings: delay conversion exception identity");
                const char *raw = "globalThis.delayRan=false;try{B.setTimeout(C.probe,{valueOf(){delayRan=true;return 0}})}catch(e){};"
                    "if(delayRan)throw Error('capture happened after delay');";
                JSValue result = JS_Eval(contexts[0], raw, strlen(raw), "raw-no-settings", JS_EVAL_TYPE_GLOBAL);
                check(!JS_IsException(result), "timer settings: missing scope refuses before delay conversion");
                if (JS_IsException(result)) JS_FreeValue(contexts[0], JS_GetException(contexts[0]));
                JS_FreeValue(contexts[0], result);
                realm_script(timer_settings_realms[0], "Promise.resolve().then(()=>{try{B.setTimeout(C.probe,0);throw Error('accepted')}"
                    "catch(e){assert(e.name==='InvalidStateError')}});", "timer settings: jobs cannot guess missing incumbent");
                check(os64_dom_timer_next(bindings[1]) == UINT64_MAX, "timer settings: refusal and cancellation leave no timer");
                for (unsigned cut = 1; cut <= 2; cut++) {
                    dom_fail_at = dom_attempts + cut;
                    const char *registration = "B.setTimeout(C.probe,0,42)";
                    check(os64_js_realm_run(timer_settings_realms[0], registration, strlen(registration), "capture-allocation-cut", &out)
                          == OS64_JS_EXCEPTION, "timer settings: record and argument allocation refusal");
                    dom_fail_at = 0;
                    check(os64_dom_timer_next(bindings[1]) == UINT64_MAX, "timer settings: allocation refusal releases capture");
                    os64_dom_script_ended(bindings[0], "capture-allocation-cut", &out);
                }
                realm_script(timer_settings_realms[0], "globalThis.rev=B.Proxy.revocable(C.probe,{});B.setTimeout(rev.proxy,0);rev.revoke();",
                    "timer settings: revoked callback retained");
                check(os64_dom_timer_fire(bindings[1], p2b_ms, &out), "timer settings: revoked callback reports and releases");
                realm_script(timer_settings_realms[0], "assert(C.errors.length===1&&B.errors.length===0&&C.errorRealms[0]);", "timer settings: report owner differs from proxy entry");
                realm_script(timer_settings_realms[0], "try{B.setTimeout(C.probe,{valueOf(){retireA();return 0}})}catch(e){"
                    "if(e.name!=='InvalidStateError')throw e}", "timer settings: delay retires captured incumbent");
                check(os64_dom_timer_next(bindings[1]) == UINT64_MAX, "timer settings: retired capture never publishes");
            } else if (retirement >= 3 && retirement < 5) {
                realm_script(timer_settings_realms[2], "globalThis.slow=()=>{while(true){}};", "timer settings: interrupted callback created");
                realm_script(timer_settings_realms[0], retirement == 3 ? "B.setInterval(C.probe,0);" : "B.setTimeout(C.slow,0);",
                             "timer settings: interrupted callback queued");
                if (retirement == 3) os64_js_cancel(runtime);
                else dom_clock_step = 200000;
                check(os64_dom_timer_fire(bindings[1], p2b_ms, &out) &&
                      (out.status == OS64_JS_CANCELLED || out.status == OS64_JS_LIMIT),
                      "timer settings: interruption keeps terminal verdict");
                dom_clock_step = 0;
                check(timer_settings_calls == 0 && JS_GetEntryContext(JS_GetRuntime(contexts[0])) == NULL &&
                      JS_GetIncumbentContext(JS_GetRuntime(contexts[0])) == NULL,
                      "timer settings: failed callback restores raw scopes");
            } else if (retirement == 5) {
                realm_script(timer_settings_realms[0], "A.setInterval(new B.Proxy(C.probe,{}),0);",
                             "timer settings: entry independent of target and queue");
                check(os64_js_realm_retire(timer_settings_realms[1], OS64_JS_ABI_ID, &out) == OS64_JS_OK,
                      "timer settings: proxy associated entry retired");
                check(os64_dom_timer_fire(bindings[0], p2b_ms, &out) && out.status == OS64_JS_OK &&
                      timer_settings_calls == 0 && os64_dom_timer_next(bindings[0]) == UINT64_MAX,
                      "timer settings: entry retirement cancels live-target live-queue interval");
            } else {
                realm_script(timer_settings_realms[0], "B.setInterval(C.probe,0);", "timer settings: retirement interval registered");
                check(os64_js_realm_retire(timer_settings_realms[retirement], OS64_JS_ABI_ID, &out) == OS64_JS_OK,
                      "timer settings: independent role retired");
                if (retirement != 1)
                    check(os64_dom_timer_fire(bindings[1], p2b_ms, &out) && out.status == OS64_JS_OK,
                          "timer settings: ineligible captured role quietly cancels interval");
                check(timer_settings_calls == 0 && os64_dom_timer_next(bindings[1]) == UINT64_MAX,
                      "timer settings: retirement suppresses body and repeated wakeups");
            }
            check(os64_js_settings_realm(runtime, OS64_JS_ABI_ID, OS64_JS_SETTINGS_ENTRY, &out) == NULL,
                  "timer settings: scope restored at host return");
            for (int i = 0; i < 3; i++) memory_balanced(bindings[i], "timer settings before drain");
            JS_RunGC(JS_GetRuntime(contexts[0]));
        }
        os64_dom_cluster_drain(cluster); os64_js_destroy(runtime); os64_dom_cluster_free(cluster);
        check(live == 0 && dom_live == 0 && !dom_allocations, "timer settings: both teardown policies release captures and proxy realms");
    }
}
