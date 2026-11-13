/* Callback reports follow callable realms, independently of activity owners. */
static void dom_callback_report_cases(void)
{
    enum { CR_TIMER, CR_FRAME, CR_IDLE, CR_IDLE_TIMEOUT, CR_MICROTASK,
           CR_MUTATION, CR_PERFORMANCE, CR_RESIZE, CR_INTERSECTION,
           CR_TIMER_JOB, CR_TIMER_REJECTION, CR_TIMER_REVOKED, CR_TIMER_REVOKED_EARLY, CR_COUNT };
    static const char *operations[] = {
        "setTimeout(A.callback,0);", "requestAnimationFrame(A.callback);",
        "requestIdleCallback(A.callback);", "requestIdleCallback(A.callback,{timeout:1});",
        "queueMicrotask(A.callback);",
        "globalThis.observer=new MutationObserver(A.callback);observer.observe(document.body,{attributes:true});document.body.id='changed';",
        "globalThis.observer=new PerformanceObserver(A.callback);observer.observe({type:'mark'});performance.mark('test',{startTime:1});",
        "globalThis.observer=new ResizeObserver(A.callback);observer.observe(document.body);",
        "globalThis.observer=new IntersectionObserver(A.callback);observer.observe(document.body);",
        "setTimeout(A.callback,0);", "setTimeout(A.callback,0);",
        "setTimeout(A.callback,0);", "setTimeout(A.callback,0);"
    };
    for (int policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; ++policy)
    for (unsigned mode = 0; mode < CR_COUNT; ++mode)
    for (unsigned retire = 0; retire < (mode >= CR_TIMER_REVOKED ? 1u : 2u); ++retire) {
        p2b_ms = 500; p2b_us = 500000;
        os64_js_config_t config = fixture_config(); config.limits.memory_bytes = 64u << 20;
        os64_js_runtime_t *runtime = NULL; os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config, policy, OS64_JS_ABI_ID, &runtime, &out) == OS64_JS_OK,
              "callback reports: runtime created");
        if (!runtime) continue;
        os64_dom_cluster_limits_t limits = {64u<<20, 16u<<20, 8u<<20, 256u<<20, 128u<<20};
        os64_dom_cluster_t *cluster = os64_dom_cluster_create(runtime, &limits, &out);
        if (!cluster) { check(false, "callback reports: cluster created"); os64_js_destroy(runtime); continue; }
        os64_js_realm_t *realms[2] = {os64_js_primary_realm(runtime, OS64_JS_ABI_ID, &out), NULL};
        check(os64_js_realm_create(runtime, OS64_JS_ABI_ID, &realms[1], &out) == OS64_JS_OK,
              "callback reports: child realm created");
        os64_dom_t *bindings[2] = {0}; ClusterProvider providers[2] = {0};
        for (unsigned i = 0; i < 2 && realms[i]; ++i) {
            DomFixture f = fixture_configured(0, 0, &config); os64_js_destroy(f.runtime);
            os64_dom_options_t options = p2b_options("https://reports.example/");
            os64_dom_admission_t a = os64_dom_create_in_realm(cluster, realms[i], f.document, f.state, &options, &out);
            bindings[i] = a.binding;
            check(a.binding && a.consumed, "callback reports: binding installed");
            if (!a.consumed) { os64_page_state_free(f.state); os64_html_document_free(f.document); }
            if (!a.binding) break;
            providers[i] = (ClusterProvider){.document=f.document, .width=100};
            os64_dom_set_resizes(a.binding, cluster_resizes, &providers[i]);
            os64_dom_set_intersections(a.binding, cluster_intersections, &providers[i]);
            os64_dom_view_provider_t view = {.opaque=&providers[i], .read=cluster_view};
            os64_dom_set_view(a.binding, &view);
        }
        if (bindings[0] && bindings[1]) {
            retirement_realms[1] = realms[1];
            for (unsigned i = 0; i < 2; ++i) {
                JSContext *ctx = os64_js_realm_context(realms[i], OS64_JS_ABI_ID, &out);
                JSValue global = JS_GetGlobalObject(ctx);
                for (unsigned j = 0; j < 2; ++j) {
                    JSContext *other = os64_js_realm_context(realms[j], OS64_JS_ABI_ID, &out);
                    check(JS_SetPropertyStr(ctx, global, j ? "B" : "A", JS_GetGlobalObject(other)) >= 0,
                          "callback reports: trusted references exported");
                }
                check(JS_SetPropertyStr(ctx, global, "retireChild", JS_NewCFunctionMagic(ctx, retirement_mark,
                      "retireChild", 0, JS_CFUNC_generic_magic, 1)) >= 0, "callback reports: native mark exported");
                JS_FreeValue(ctx, global);
                realm_script(realms[i], STORAGE_ASSERT
                    "globalThis.errors=[];globalThis.rejections=[];"
                    "addEventListener('error',e=>errors.push([e.error&&e.error.message,e.filename]));"
                    "addEventListener('unhandledrejection',e=>rejections.push(e.reason.message));",
                    "callback reports: independent error ledgers");
            }
            char name[64], other[64];
            check(os64_dom_script_name(bindings[0], "https://reports.example/reserved.js", name, sizeof(name)) &&
                  os64_dom_script_name(bindings[0], "https://reports.example/callback.js", name, sizeof(name)) &&
                  os64_dom_script_name(bindings[1], "https://foreign.example/muted.js", other, sizeof(other)),
                  "callback reports: distinct script tables");
            char source[240];
            snprintf(source, sizeof(source), "globalThis.callback=()=>{%s%s};", retire ? "retireChild();" : "",
                mode == CR_TIMER_JOB ? "queueMicrotask(()=>{throw Error('callback')});" :
                mode == CR_TIMER_REJECTION ? "Promise.reject(Error('callback'));" : "throw Error('callback');");
            realm_script(realms[0], source, name);
            if (mode < CR_TIMER_REVOKED)
                check(os64_dom_script_name(bindings[0], "https://foreign.example/later.js", other, sizeof(other)),
                      "callback reports: last script differs from retained callback");
            if (mode >= CR_TIMER_REVOKED)
                realm_script(realms[0], "globalThis.rev=Proxy.revocable(callback,{});callback=rev.proxy;",
                             "callback reports: revocable foreign callback");
            if (mode == CR_TIMER_REVOKED_EARLY)
                realm_script(realms[0], "rev.revoke();", "callback reports: revoke before registration");
            os64_js_status_t status = os64_js_realm_run(realms[1], operations[mode], strlen(operations[mode]), "schedule", &out);
            check(status == (mode == CR_MICROTASK ? OS64_JS_EXCEPTION : OS64_JS_OK),
                  "callback reports: schedule and automatic checkpoint complete");
            check(os64_dom_script_ended(bindings[1], "schedule", &out), "callback reports: script reports settled");
            if (mode == CR_TIMER_REVOKED)
                realm_script(realms[0], "rev.revoke();", "callback reports: revoke after registration");
            bool fired = true;
            if (mode == CR_TIMER || mode == CR_IDLE_TIMEOUT || mode == CR_PERFORMANCE || mode >= CR_TIMER_JOB)
                fired = os64_dom_timer_fire(bindings[1], 1000, &out);
            else if (mode == CR_FRAME) fired = os64_dom_animation_fire(bindings[1], 1000, &out);
            else if (mode == CR_IDLE) fired = os64_dom_idle_run(bindings[1], &out);
            else if (mode == CR_RESIZE) fired = os64_dom_resizes_step(bindings[1], &out);
            else if (mode == CR_INTERSECTION)
                fired = os64_dom_intersections_update(bindings[1]) && os64_dom_intersections_fire(bindings[1], &out);
            check(fired && (out.status == OS64_JS_OK || out.status == OS64_JS_EXCEPTION ||
                           out.status == OS64_JS_UNHANDLED_REJECTION),
                  "callback reports: delivery completes without shared failure");
            char label[100]; snprintf(label, sizeof(label), "callback reports: mode %u retire %u routes callable error", mode, retire);
            realm_script(realms[0], mode == CR_TIMER_REVOKED_EARLY
                ? "assert(errors.length===0&&B.errors.length===1,'unknown revoked realm uses registration fallback');"
                : mode == CR_TIMER_REVOKED
                ? "assert(errors.length===1&&B.errors.length===0&&errors[0][0].includes('revoked'),'cached revoked owner');"
                  "assert(errors[0][1]==='https://reports.example/callback.js','cached revoked script');"
                : mode == CR_TIMER_REJECTION
                ? "assert(rejections.join()==='callback'&&B.rejections.length===0&&errors.length===0,'wrong rejection owner');"
                : "assert(errors.length===1&&B.errors.length===0,'wrong report owner');"
                  "assert(errors[0][0]==='callback'&&errors[0][1]==='https://reports.example/callback.js','wrong script table');", label);
            check(!os64_dom_take_report(bindings[0], &out) && !os64_dom_take_report(bindings[1], &out),
                  "callback reports: shared task close leaves no sibling diagnostic debt");
        }
        os64_dom_cluster_drain(cluster); os64_js_destroy(runtime); os64_dom_cluster_free(cluster);
        check(live == 0 && dom_live == 0 && !dom_allocations, "callback reports: native and engine holds released");
    }
}
