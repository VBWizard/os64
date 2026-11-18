/* Prepare before the first script. Raw globals exchanged by the trusted host
 * are independent identity oracles, not a browser publication surface. */
static bool identity_open(CeFixture *f, int policy, DBrowsingContext **contexts)
{
    if (!authority_bindings_open(f, policy, 0)) return false;
    os64_js_outcome_t out;
    for (unsigned i = 0; i < 5; i++) {
        contexts[i] = d_browsing_context_create(f->cluster, i ? contexts[0] : NULL,
            f->bindings[i], "identity", &out);
        if (!contexts[i] || d_window_identity_prepare(ce_contexts[i], f->bindings[i], contexts[i]) < 0)
            return false;
        check(d_window_identity_prepare(ce_contexts[i], f->bindings[i], contexts[i]) == 0,
            "identity: preparation idempotent");
    }
    for (unsigned i = 0; i < 5; i++) {
        JSContext *ctx = ce_contexts[i];
        JSValue global = JS_GetGlobalObject(ctx);
        for (unsigned j = 0; j < 5; j++) {
            char raw[] = {(char)('A' + j), 0}, proxy[] = {'P', (char)('A' + j), 0};
            check(JS_SetPropertyStr(ctx, global, raw, JS_GetGlobalObject(ce_contexts[j])) >= 0 &&
                  JS_SetPropertyStr(ctx, global, proxy, d_window_proxy(ctx, contexts[j])) >= 0,
                  "identity: trusted independent raw/proxy exchange");
        }
        JS_FreeValue(ctx, global);
        realm_script(ce_realms[i], STORAGE_ASSERT
            "function denied(f){let e;try{f()}catch(x){e=x}assert(e&&e.name==='SecurityError')}"
            "function typeError(f){let e;try{f()}catch(x){e=x}assert(e&&e.name==='TypeError')}",
            "identity: first script setup");
    }
    return true;
}

static void identity_failure_cases(void)
{
    for (int policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; policy++) {
        for (unsigned mode = 0; mode < 2; mode++) {
            CeFixture f;
            if (!authority_bindings_open(&f, policy, 0)) { ce_close(&f); continue; }
            os64_js_outcome_t out;
            DBrowsingContext *context = d_browsing_context_create(f.cluster, NULL, f.bindings[0], "failure", &out);
            realm_script(ce_realms[0], STORAGE_ASSERT
                "var touched=0;var raw=this;", "identity refusal: fixture sentinel");
            realm_script(ce_realms[0], mode
                ? "Object.defineProperty(this,'globalThis',{value:17,writable:false,configurable:false})"
                : "Object.defineProperty(this,'globalThis',{get(){touched++;return 17},configurable:true})",
                "identity refusal: altered bootstrap descriptor");
            check(d_window_identity_prepare(ce_contexts[0], f.bindings[0], context) < 0 &&
                  !f.bindings[0]->window_identity && !context->proxy,
                  "identity refusal: no partial engine identity or proxy publication");
            JS_FreeValue(ce_contexts[0], JS_GetException(ce_contexts[0]));
            realm_script(ce_realms[0], "assert(this===raw&&touched===0&&(function(){return this})()===raw)",
                "identity refusal: getters not invoked and engine receiver unchanged");
            ce_close(&f);
        }
        for (unsigned native = 0; native < 2; native++) {
            size_t count = 0;
            for (size_t cut = 0; cut <= count; cut++) {
                CeFixture f;
                if (!authority_bindings_open(&f, policy, 0)) { ce_close(&f); continue; }
                os64_js_outcome_t out;
                DBrowsingContext *context = d_browsing_context_create(f.cluster, NULL, f.bindings[0], "cut", &out);
                JSValue raw = JS_GetGlobalObject(ce_contexts[0]);
                check(JS_SetPropertyStr(ce_contexts[0], raw, "raw", JS_DupValue(ce_contexts[0], raw)) >= 0,
                    "identity cuts: independent raw oracle");
                JS_FreeValue(ce_contexts[0], raw);
                size_t before = native ? dom_attempts : attempts;
                if (native) dom_fail_at = cut ? before + cut : 0;
                else fail_at = cut ? before + cut : 0;
                int result = d_window_identity_prepare(ce_contexts[0], f.bindings[0], context);
                size_t used = (native ? dom_attempts : attempts) - before;
                fail_at = dom_fail_at = 0;
                if (!cut) { count = used; check(result == 0, "identity cuts: measure preparation"); }
                else {
                    check(result < 0 && !f.bindings[0]->window_identity, "identity cuts: preparation refused");
                    JS_FreeValue(ce_contexts[0], JS_GetException(ce_contexts[0]));
                    JSValue global = JS_GetGlobalObject(ce_contexts[0]);
                    JSValue visible = JS_GetPropertyStr(ce_contexts[0], global, "globalThis");
                    check(JS_IsObject(visible) && JS_VALUE_GET_PTR(visible) == JS_VALUE_GET_PTR(global),
                        "identity cuts: globalThis property remains raw");
                    JS_FreeValue(ce_contexts[0], visible);
                    JS_FreeValue(ce_contexts[0], global);
                    /* Engine OOM follows the runtime's failure policy. Native
                     * DOM refusal can be retried without masking that policy. */
                    if (native) {
                        realm_script(ce_realms[0], STORAGE_ASSERT "assert(this===raw&&globalThis===raw)",
                            "identity cuts: native refusal preserves engine receiver");
                        check(d_window_identity_prepare(ce_contexts[0], f.bindings[0], context) == 0,
                            "identity cuts: native refusal retry succeeds");
                    }
                }
                if (!cut || native)
                    realm_script(ce_realms[0], STORAGE_ASSERT "assert(this!==raw&&this===globalThis&&this===window)",
                        "identity cuts: successful identity is coherent");
                ce_close(&f);
                check(live == 0 && dom_live == 0 && !dom_allocations, "identity cuts: balanced teardown");
            }
            printf("Window identity cuts policy %d native %u: %zu\n", policy, native, count);
        }
    }
}

static void dom_window_identity_cases(void)
{
    identity_failure_cases();
    for (int policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; policy++) {
        CeFixture f; DBrowsingContext *c[5]; os64_js_outcome_t out;
        if (!identity_open(&f, policy, c)) { check(false, "identity: fixture"); ce_close(&f); continue; }
        realm_script(ce_realms[0],
            "assert(this===PA&&globalThis===PA&&window===PA&&self===PA&&frames===PA&&A!==PA);"
            "assert(document.defaultView===PA&&PC.document.defaultView===PC);"
            "assert((function(){return this})()===PA&&(0,eval)('this')===PA&&eval('this')===PA);"
            "assert(Function('return this')()===PA&&C.Function('return this')()===PC);"
            "assert((function(){'use strict';return this})()===undefined);"
            "assert(Reflect.apply(function(){return this},null,[])===PA);"
            "assert(Object.getOwnPropertyDescriptor(A,'globalThis').value===PA);"
            "var identityVar=17;assert(A.identityVar===17&&PA.identityVar===17);PA.identityVar=18;assert(identityVar===18);"
            "globalThis.savedDocument=document;globalThis.savedChild=PC.document;"
            "globalThis.viewGetter=Object.getOwnPropertyDescriptor(Document.prototype,'defaultView').get;"
            "assert(viewGetter.call(PC.document)===PC&&viewGetter.call(document.implementation.createHTMLDocument())===null);"
            "assert(new UIEvent('test',{view:PB}).view===PB&&new UIEvent('test',{view:C}).view===PC);"
            "assert(new MouseEvent('test',{relatedTarget:PC}).relatedTarget===PC);"
            "assert(new MessageEvent('test',{source:PB}).source===PB&&new MessageEvent('test',{source:C}).source===PC);"
            "assert(new FocusEvent('test',{relatedTarget:B}).relatedTarget===PB);"
            "let traps=0, fake=new Proxy(PA,{get(){traps++;throw 1},getPrototypeOf(){traps++;throw 1}});"
            "typeError(()=>new UIEvent('test',{view:fake}));typeError(()=>new MouseEvent('test',{relatedTarget:fake}));assert(traps===0);",
            "identity: global engines, storage, document receivers and UI conversion");
        realm_script(ce_realms[0],
            "globalThis.eventCalls=0;globalThis.hostCalls=0;globalThis.timerCalls=0;globalThis.frameCalls=0;"
            "addEventListener('identity',function(e){'use strict';assert(this===PA&&e.target===PA&&e.currentTarget===PA);"
                "assert(e.composedPath().length===1&&e.composedPath()[0]===PA);eventCalls++});"
            "let objectListener={handleEvent(e){assert(this===objectListener&&e.currentTarget===PA);eventCalls++}};"
            "addEventListener('identity',objectListener);PA.dispatchEvent(new Event('identity'));assert(eventCalls===2);"
            "addEventListener('bubble',function(e){assert(this===PA&&e.currentTarget===PA&&e.target===document.body);"
                "assert(e.composedPath().slice(-1)[0]===PA);eventCalls++});"
            "document.body.dispatchEvent(new Event('bubble',{bubbles:true}));assert(eventCalls===3);"
            "document.body.onload=function(e){'use strict';assert(this===PA&&e.currentTarget===PA);hostCalls++};"
            "addEventListener('mousemove',function(e){assert(e.view===PA&&e.currentTarget===PA);hostCalls++});"
            "addEventListener('host',function(e){'use strict';assert(this===PA&&e.target===PA&&e.currentTarget===PA);hostCalls++});"
            "onerror=function(){'use strict';assert(this===PA);eventCalls++;return true};"
            "dispatchEvent(new ErrorEvent('error',{message:'identity'}));assert(eventCalls===4);"
            "setTimeout(function(x){'use strict';assert(this===PA&&x===42);timerCalls++},0,42);"
            "requestAnimationFrame(function(){assert(this===PA);frameCalls++});"
            "requestAnimationFrame(function(){'use strict';assert(this===undefined);frameCalls++});"
            "denied(()=>Reflect.apply(A.addEventListener,PB,['bad',()=>{}]));",
            "identity: script events, reflected handlers, foreign target denial and scheduled receivers");
        bool prevented = false;
        os64_dom_event_t host = {.type="host"}, load = {.type="load"};
        check(os64_dom_dispatch(f.bindings[0], NULL, &host, &prevented, &out) == OS64_JS_OK &&
              os64_dom_dispatch(f.bindings[0], NULL, &load, &prevented, &out) == OS64_JS_OK,
              "identity: host Window dispatch");
        os64_dom_event_t move = {.type="mousemove", .kind=OS64_DOM_EVENT_MOUSE};
        check(os64_dom_dispatch(f.bindings[0], NULL, &move, &prevented, &out) == OS64_JS_OK,
              "identity: host UI view");
        check(os64_dom_timer_fire(f.bindings[0], os64_dom_timer_next(f.bindings[0]), &out) && out.status == OS64_JS_OK,
              "identity: timer invocation");
        check(os64_dom_animation_fire(f.bindings[0], 1000, OS64_DOM_FRAME_NOW, &out) && out.status == OS64_JS_OK,
              "identity: animation invocation");
        realm_script(ce_realms[0], "assert(hostCalls===3&&timerCalls===1&&frameCalls===2)",
            "identity: host callback evidence");
        realm_script(ce_realms[2],
            "globalThis.oldEvents=0;addEventListener('replace',()=>{oldEvents++});",
            "identity: outgoing document listener");
        os64_dom_child_t child = location_candidate(&f, 0, "https://replacement.test/identity");
        check(d_browsing_context_replace(c[2], c[2]->generation, child.binding, &out) == OS64_JS_BAD_ARGUMENT &&
              c[2]->active == f.bindings[2] && d_delivery_active(f.bindings[2]),
              "identity: unprepared replacement refuses before retirement");
        JSContext *ctx = os64_js_realm_context(child.binding->realm, OS64_JS_ABI_ID, &out);
        check(d_window_identity_prepare(ce_contexts[0], child.binding, c[2]) < 0 && !child.binding->window_identity,
              "identity: wrong engine context refuses");
        JS_FreeValue(ce_contexts[0], JS_GetException(ce_contexts[0]));
        check(d_window_identity_prepare(ctx, child.binding, c[2]) == 0, "identity: replacement preparation");
        check(d_browsing_context_replace(c[1], c[1]->generation, child.binding, &out) == OS64_JS_BAD_ARGUMENT &&
              d_delivery_active(f.bindings[1]), "identity: preparation cannot publish to another context");
        size_t attempts = dom_attempts;
        check(d_browsing_context_replace(c[2], c[2]->generation, child.binding, &out) == OS64_JS_OK &&
              dom_attempts == attempts, "identity: allocation-free publication");
        realm_script(child.binding->realm, STORAGE_ASSERT
            "assert(this===window&&globalThis===window&&document.defaultView===window);"
            "globalThis.newEvents=0;addEventListener('replace',function(e){assert(this===window&&e.target===window);newEvents++});",
            "identity: replacement first script");
        realm_script(ce_realms[0],
            "assert(savedChild.defaultView===PC&&PC.document!==savedChild&&viewGetter.call(savedChild)===PC);"
            "PC.dispatchEvent(new PC.Event('replace'));assert(PC.newEvents===1&&C.oldEvents===0);"
            "assert(PC!==C&&C.globalThis===PC);",
            "identity: retained document identity, current target, isolated outgoing listeners");
        os64_dom_child_t cross = location_candidate(&f, 1, "https://replacement.test/cross");
        JSContext *cross_ctx = os64_js_realm_context(cross.binding->realm, OS64_JS_ABI_ID, &out);
        check(d_window_identity_prepare(cross_ctx, cross.binding, c[2]) == 0 &&
              d_browsing_context_replace(c[2], c[2]->generation, cross.binding, &out) == OS64_JS_OK,
              "identity: cross-origin replacement prepared and published");
        realm_script(ce_realms[0],
            "assert(savedChild.defaultView===PC&&C.globalThis===PC);denied(()=>PC.document);"
            "assert(new MessageEvent('test',{source:PC}).source===PC);",
            "identity: stable identity grants no cross-origin property authority");
        check(d_browsing_context_discard(c[2], c[2]->generation, &out) == OS64_JS_OK,
            "identity: discard replacement");
        realm_script(ce_realms[0], "assert(savedChild.defaultView===PC&&PC.closed)",
            "identity: document identity survives discard");
        realm_script(ce_realms[0],
            "globalThis=23;assert(this===PA&&(function(){return this})()===PA);"
            "assert(delete A.globalThis&&this===PA&&(0,eval)('this')===PA);"
            "self=17;frames=18;assert(window===PA&&self===17&&frames===18&&document.defaultView===PA);"
            "let wd=Object.getOwnPropertyDescriptor(A,'window');let dd=Object.getOwnPropertyDescriptor(A,'document');"
            "assert(!wd.configurable&&!dd.configurable&&!wd.set&&!dd.set);",
            "identity: mutable aliases do not replace the engine identity or unforgeable members");
        ce_close(&f);
        check(live == 0 && dom_live == 0 && !dom_allocations, "identity: both teardown policies balance ownership");
    }
}
