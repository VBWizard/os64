/* Native bytes/length/generation and named property lookup are independent
 * oracles for the private Window.name accessor and publication behavior. */
typedef struct { JSValue setter, receiver, value; size_t cut, allocations; } WindowNameCut;

static JSValue window_name_cut(JSContext *ctx, void *opaque)
{
    WindowNameCut *probe = opaque;
    size_t before = attempts;
    fail_at = probe->cut ? attempts + probe->cut : 0;
    JSValue result = JS_Call(ctx, probe->setter, probe->receiver, 1, &probe->value);
    fail_at = 0;
    probe->allocations = attempts - before;
    return result;
}

static void window_name_failure_cases(void)
{
    for (int policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; policy++) {
        size_t count = 0;
        for (size_t cut = 0; cut <= count; cut++) {
            CeFixture f; DBrowsingContext *c[5]; os64_js_outcome_t out;
            if (!identity_open(&f, policy, c)) { check(false, "Window.name cuts: fixture"); ce_close(&f); continue; }
            JSContext *ctx = ce_contexts[0];
            JSValue global = JS_GetGlobalObject(ctx);
            JSAtom atom = JS_NewAtom(ctx, "name"); JSPropertyDescriptor desc;
            check(JS_GetOwnProperty(ctx, &desc, global, atom) == 1, "Window.name cuts: own accessor");
            JS_FreeAtom(ctx, atom);
            WindowNameCut probe = {desc.setter, global, JS_NewString(ctx, "caf\xc3\xa9"), cut, 0};
            check(os64_js_realm_task_begin(ce_realms[0], OS64_JS_ABI_ID, "Window name cut", &out) == OS64_JS_OK,
                "Window.name cuts: checked task");
            JSValue result = JS_UNDEFINED;
            os64_js_status_t status = os64_js_realm_with_settings(ce_realms[0], ce_realms[0], OS64_JS_ABI_ID,
                window_name_cut, &probe, &result, &out);
            if (!cut) {
                count = probe.allocations;
                check(status == OS64_JS_OK && !JS_IsException(result) &&
                    c[0]->name_length == 5 && !memcmp(c[0]->name, "caf\xc3\xa9", 5), "Window.name cuts: measured conversion succeeds");
            } else check((status != OS64_JS_OK || JS_IsException(result)) &&
                c[0]->name_length == 8 && !memcmp(c[0]->name, "identity", 8), "Window.name cuts: conversion failure preserves bytes");
            if (JS_IsException(result)) JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, result); JS_FreeValue(ctx, probe.value);
            JS_FreeValue(ctx, desc.value); JS_FreeValue(ctx, desc.getter); JS_FreeValue(ctx, desc.setter); JS_FreeValue(ctx, global);
            check(os64_js_task_end(ce_runtime, OS64_JS_ABI_ID, &out) != OS64_JS_BUSY, "Window.name cuts: loans unwind");
            ce_close(&f);
            check(live == 0 && dom_live == 0 && !dom_allocations, "Window.name cuts: balanced teardown");
        }
        printf("Window.name conversion cuts policy %d: %zu\n", policy, count);
    }
}

static void dom_window_name_cases(void)
{
    window_name_failure_cases();
    for (int policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; policy++) {
        CeFixture f; DBrowsingContext *c[5]; os64_js_outcome_t out;
        if (!identity_open(&f, policy, c)) { check(false, "Window.name: fixture"); ce_close(&f); continue; }
        size_t charged = f.cluster->bytes, before = dom_attempts;
        uint64_t generation = c[2]->generation;
        check(!d_browsing_context_rename(c[2], generation + 1, f.bindings[2], "bad", 3) &&
              !d_browsing_context_rename(c[2], generation, f.bindings[0], "bad", 3),
              "Window.name: stale publication and wrong target refuse");
        check(d_browsing_context_rename(c[2], generation, f.bindings[2], "native", 6) &&
              f.cluster->bytes == charged && dom_attempts == before && c[2]->generation == generation,
              "Window.name: native rename is allocation-free and preserves publication identity");
        realm_script(ce_realms[0],
            "assert(PC.name==='native'&&window.native===PC);"
            "let d=Object.getOwnPropertyDescriptor(window,'name');"
            "assert(d.enumerable&&d.configurable&&typeof d.get==='function'&&typeof d.set==='function');"
            "Reflect.apply(d.set,PC,['renamed']);assert(Reflect.apply(d.get,PC,[])==='renamed'&&window.renamed===PC&&window.native===undefined);"
            "let conversions=0,v={toString(){conversions++;return 'converted'}};"
            "denied(()=>Reflect.apply(d.set,PB,[v]));denied(()=>PB.name);assert(conversions===0);"
            "typeError(()=>Reflect.apply(d.set,{},[v]));assert(conversions===0);"
            "let traps=0,fake=new Proxy(PC,{get(){traps++;throw 1},getPrototypeOf(){traps++;throw 2}});"
            "typeError(()=>Reflect.apply(d.set,fake,[v]));assert(traps===0&&conversions===0);"
            "PC.name={toString(){PC.name='inner';return 'outer'}};assert(PC.name==='outer'&&window.outer===PC);"
            "let marker={};try{PC.name={toString(){throw marker}};assert(false)}catch(e){assert(e===marker)}assert(PC.name==='outer');"
            "typeError(()=>{PC.name=Symbol('bad')});assert(PC.name==='outer');"
            "for(let n of ['','a\\0b','\\0','caf\xc3\xa9','\\ud800','\\udc00','\\ud83d\\ude00','then']){"
            "PC.name=n;assert(PC.name===n);if(n)assert(window[n]===PC);else assert(window['']===undefined);}"
            "assert(window.a===undefined);PC.name='n'.repeat(255);assert(PC.name.length===255);"
            "let error;try{PC.name='n'.repeat(256)}catch(e){error=e}assert(error&&error.name==='QuotaExceededError'&&PC.name.length===255);"
            "PC.name=null;assert(PC.name==='null');PC.name=undefined;assert(PC.name==='undefined');PC.name='stable';",
            "Window.name: descriptor, conversions, receiver authority, exact strings, bounds and nested rename");
        check(d_browsing_context_name_length(c[2]) == 6 && !memcmp(d_browsing_context_name(c[2]), "stable", 6) &&
              c[2]->generation == generation, "Window.name: independent native bytes and unchanged generation");
        realm_script(ce_realms[1], "PB.name='stable'", "Window.name: first duplicate changed by its own origin");
        realm_script(ce_realms[0], "assert(window.stable===undefined);PC.name='uniqueChild';assert(window.uniqueChild===PC)",
            "Window.name: first foreign duplicate suppresses later same-origin child after rename");
        realm_script(ce_realms[2], "PC.name='uniqueChild'", "Window.name: child owns its name");
        realm_script(ce_realms[1], "assert(PA.uniqueChild===PC)", "Window.name: restricted foreign lookup sees renamed same-origin child");
        os64_dom_child_t next = location_candidate(&f, 0, "https://name.test/replacement");
        JSContext *ctx = os64_js_realm_context(next.binding->realm, OS64_JS_ABI_ID, &out);
        check(d_window_identity_prepare(ctx, next.binding, c[2]) == 0 &&
              d_browsing_context_replace(c[2], generation, next.binding, &out) == OS64_JS_OK,
              "Window.name: same-origin child publication");
        realm_script(ce_realms[0], "assert(PC.name==='uniqueChild'&&window.uniqueChild===PC&&C.name==='uniqueChild');denied(()=>Reflect.apply(d.get,C,[]))",
            "Window.name: global property receiver follows proxy; explicit departed member receiver refuses");
        os64_dom_child_t foreign = location_candidate(&f, 1, "https://name.test/foreign");
        ctx = os64_js_realm_context(foreign.binding->realm, OS64_JS_ABI_ID, &out);
        check(d_window_identity_prepare(ctx, foreign.binding, c[2]) == 0 &&
              d_browsing_context_replace(c[2], c[2]->generation, foreign.binding, &out) == OS64_JS_OK,
              "Window.name: foreign nested publication");
        realm_script(foreign.binding->realm, STORAGE_ASSERT "assert(name==='uniqueChild')",
            "Window.name: nested context preserves name across origin change");
        realm_script(ce_realms[0], "denied(()=>PC.name);assert(window.uniqueChild===undefined)",
            "Window.name: origin change grants no property authority");
        ce_close(&f);
        check(live == 0 && dom_live == 0 && !dom_allocations, "Window.name: balanced nested teardown");

        if (!identity_open(&f, policy, c)) { check(false, "Window.name: root fixture"); ce_close(&f); continue; }
        realm_script(ce_realms[0], "name='rootSecret'", "Window.name: root state");
        next = location_candidate(&f, 0, "https://name.test/root-same");
        ctx = os64_js_realm_context(next.binding->realm, OS64_JS_ABI_ID, &out);
        check(d_window_identity_prepare(ctx, next.binding, c[0]) == 0 &&
              d_browsing_context_replace(c[0], c[0]->generation, next.binding, &out) == OS64_JS_OK,
              "Window.name: same-origin root replacement");
        realm_script(next.binding->realm, STORAGE_ASSERT "assert(name==='rootSecret')", "Window.name: same-origin root preserves name");
        foreign = location_candidate(&f, 1, "https://name.test/root-foreign");
        ctx = os64_js_realm_context(foreign.binding->realm, OS64_JS_ABI_ID, &out);
        check(d_window_identity_prepare(ctx, foreign.binding, c[0]) == 0 &&
              d_browsing_context_replace(c[0], c[0]->generation, foreign.binding, &out) == OS64_JS_OK,
              "Window.name: cross-origin root replacement");
        realm_script(foreign.binding->realm, STORAGE_ASSERT "assert(name==='')", "Window.name: origin-changing root clears name");
        check(d_browsing_context_name_length(c[0]) == 0, "Window.name: native reset oracle");
        ce_close(&f);
        check(live == 0 && dom_live == 0 && !dom_allocations, "Window.name: balanced root teardown");

        for (unsigned retired = 0; retired < 2; retired++) {
            if (!identity_open(&f, policy, c)) { check(false, "Window.name: reentry fixture"); ce_close(&f); continue; }
            JSValue global = JS_GetGlobalObject(ce_contexts[0]);
            check(JS_SetPropertyStr(ce_contexts[0], global, "retireNameRealm",
                JS_NewCFunctionMagic(ce_contexts[0], ce_retire, "retireNameRealm", 0,
                    JS_CFUNC_generic_magic, retired ? 2 : 0)) >= 0, "Window.name: retirement seam");
            JS_FreeValue(ce_contexts[0], global);
            realm_script(ce_realms[0], "PC.name='before';denied(()=>{PC.name={toString(){retireNameRealm();return 'after'}}})",
                "Window.name: conversion retirement rechecks caller and target");
            check(c[2]->name_length == 6 && !memcmp(c[2]->name, "before", 6),
                "Window.name: retirement cannot commit native bytes");
            ce_close(&f);
            check(live == 0 && dom_live == 0 && !dom_allocations, "Window.name: balanced reentry teardown");
        }
        if (authority_bindings_open(&f, policy, 0)) {
            realm_script(ce_realms[0], STORAGE_ASSERT
                "assert(name==='');let n=0;name={toString(){n++;return 'unassociated'}};assert(n===1&&name==='');",
                "Window.name: live unassociated Window converts then ignores assignment");
        } else check(false, "Window.name: unassociated fixture");
        ce_close(&f);
        check(live == 0 && dom_live == 0 && !dom_allocations, "Window.name: balanced unassociated teardown");
    }
}
