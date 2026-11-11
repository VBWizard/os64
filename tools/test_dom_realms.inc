/* Explicit binding realm routing. A secondary realm owns the DOM while the
 * primary keeps an independent exception host and pending rejection queue.
 * This is not the cross-binding adoption/cluster fixture. */
static unsigned realm_primary_errors;
static void realm_primary_error(void *opaque, JSContext *ctx, JSValue error)
{
    (void)ctx; (void)error;
    check(opaque == &realm_primary_errors, "realm: primary provider identity");
    realm_primary_errors++;
}
static bool realm_script(os64_js_realm_t *realm, const char *source, const char *name)
{
    os64_js_outcome_t out;
    os64_js_status_t status = os64_js_realm_run(realm, source, strlen(source), name, &out);
    if (status != OS64_JS_OK)
        fprintf(stderr, "realm script %s: status=%d %s\n", name, status, out.message);
    check(status == OS64_JS_OK, name);
    return status == OS64_JS_OK;
}
static void dom_realm_cases(void)
{
    for (int policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; policy++) {
        os64_js_config_t config = fixture_config();
        DomFixture f = fixture_configured(0, 0, &config);
        os64_js_destroy(f.runtime);
        f.runtime = NULL;
        os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config, policy, OS64_JS_ABI_ID, &f.runtime, &out) == OS64_JS_OK,
              "realm: configured runtime");
        os64_js_realm_t *primary = os64_js_primary_realm(f.runtime, OS64_JS_ABI_ID, &out), *child = NULL;
        check(os64_js_realm_create(f.runtime, OS64_JS_ABI_ID, &child, &out) == OS64_JS_OK,
              "realm: secondary created");
        realm_primary_errors = 0;
        os64_js_script_host_t host = {.opaque = &realm_primary_errors, .exception = realm_primary_error};
        check(os64_js_realm_set_script_host(primary, OS64_JS_ABI_ID, &host, &out) == OS64_JS_OK,
              "realm: independent primary exception host");
        p2b_ms = 1000; p2b_us = 1000000;
        os64_dom_options_t options = p2b_options("https://child.example/page");
        os64_js_runtime_t *foreign = create_fixture(&config);
        os64_js_realm_t *foreign_realm = os64_js_primary_realm(foreign, OS64_JS_ABI_ID, &out);
        size_t before = dom_attempts;
        check(d_create_in_realm(f.runtime, foreign_realm, f.document, f.state, &options, &out) == NULL &&
              out.status == OS64_JS_BAD_ARGUMENT && dom_attempts == before,
              "realm: foreign-runtime installation refused before native allocation");
        os64_js_destroy(foreign);
        f.binding = d_create_in_realm(f.runtime, child, f.document, f.state, &options, &out);
        check(f.binding != NULL && out.status == OS64_JS_OK, "realm: DOM installs in secondary");
        if (!f.binding) { fixture_free(&f); continue; }
        realm_script(primary, "if(typeof document!=='undefined'||typeof setTimeout!=='undefined')throw Error('primary polluted')",
                     "realm: installation leaves primary globals alone");
        realm_script(child, STORAGE_ASSERT
            "assert(document.URL==='https://child.example/page');globalThis.seen=[];"
            "setTimeout(function(){assert(this===window);seen.push('timer');"
            "document.body.setAttribute('data-timer','child')},0);"
            "setTimeout(\"seen.push('string')\",0);"
            "requestAnimationFrame(function(){assert(this===window);seen.push('frame')});"
            "requestIdleCallback(()=>seen.push('idle'));"
            "addEventListener('resize',()=>{seen.push('event');document.body.dataset.event='child'});"
            "globalThis.observed=0;new MutationObserver(()=>observed++).observe(document.body,{attributes:true});",
            "realm: callbacks installed in secondary");
        check(os64_dom_timer_fire(f.binding, p2b_ms, &out) && out.status == OS64_JS_OK,
              "realm: host timer enters secondary");
        check(os64_dom_timer_fire(f.binding, p2b_ms, &out) && out.status == OS64_JS_OK,
              "realm: string timer evaluates in secondary");
        p2b_advance(20);
        check(os64_dom_animation_fire(f.binding, p2b_ms, &out) && out.status == OS64_JS_OK,
              "realm: host animation enters secondary");
        check(os64_dom_idle_run(f.binding, &out) && out.status == OS64_JS_OK,
              "realm: host idle enters secondary");
        os64_dom_event_t event = {.type = "resize"};
        bool prevented = false;
        check(os64_dom_dispatch(f.binding, NULL, &event, &prevented, &out) == OS64_JS_OK,
              "realm: host event enters secondary");
        realm_script(child, "assert(seen.join()==='timer,string,frame,idle,event',seen.join());"
            "assert(observed===2&&document.body.dataset.timer==='child'&&document.body.dataset.event==='child');"
            "globalThis.errors=0;globalThis.rejections=[];"
            "addEventListener('error',e=>{errors++;e.preventDefault()});"
            "addEventListener('unhandledrejection',e=>{rejections.push(e.reason);e.preventDefault()})",
            "realm: callbacks and observer use secondary DOM");
        const char *thrown = "throw Error('child failure')";
        check(os64_js_realm_run(child, thrown, strlen(thrown), "child-error.js", &out) == OS64_JS_EXCEPTION,
              "realm: child exception reported");
        check(os64_dom_script_ended(f.binding, "child-error.js", &out), "realm: child error delivery completes");
        realm_script(child, "assert(errors===1)", "realm: child error reaches child listener");
        check(realm_primary_errors == 0, "realm: child error bypasses primary provider");
        realm_script(primary, "Promise.reject('primary debt')", "realm: primary rejection queued");
        const char *rejected = "Promise.reject('child debt')";
        check(os64_js_realm_run(child, rejected, strlen(rejected), "child-rejection.js", &out) == OS64_JS_OK,
              "realm: child rejection queued");
        check(os64_dom_script_ended(f.binding, "child-rejection.js", &out), "realm: child rejection delivery completes");
        realm_script(child, "assert(rejections.join()==='child debt',rejections.join())", "realm: child takes its rejection");
        JSValue promise, reason; uint32_t tag;
        bool taken = os64_js_realm_take_rejection(primary, OS64_JS_ABI_ID, &promise, &reason, &tag);
        check(taken, "realm: primary rejection survives child delivery");
        if (taken) {
            JSContext *ctx = os64_js_realm_context(primary, OS64_JS_ABI_ID, &out);
            const char *text = JS_ToCString(ctx, reason);
            check(text && !strcmp(text, "primary debt"), "realm: retained rejection belongs to primary");
            JS_FreeCString(ctx, text); JS_FreeValue(ctx, promise); JS_FreeValue(ctx, reason);
        }
        check(os64_dom_create(f.runtime, f.document, f.state, &options, &out) == NULL && out.status == OS64_JS_BAD_ARGUMENT,
              "realm: duplicate installation rejected before changing primary");
        realm_script(child, "assert(document.body.dataset.timer==='child')", "realm: duplicate refusal preserves binding");
        memory_balanced(f.binding, "realm before drain");
        os64_dom_drain(f.binding);
        memory_balanced(f.binding, "realm after drain");
        thrown = "throw Error('primary still configured')";
        check(os64_js_realm_run(primary, thrown, strlen(thrown), "primary-error.js", &out) == OS64_JS_EXCEPTION &&
              realm_primary_errors == 1, "realm: child drain preserves primary exception host");
        os64_js_teardown_report_t report;
        os64_js_destroy_report(f.runtime, &report);
        os64_dom_free(f.binding);
        os64_page_state_free(f.state);
        os64_html_document_free(f.document);
        check(live == 0 && dom_live == 0 && !dom_allocations, "realm: both teardown policies release native owners");
        check(policy == OS64_JS_TEARDOWN_RECLAIM || !report.leaked, "realm: finalizer teardown has no leftovers");
    }
}
