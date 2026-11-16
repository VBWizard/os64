/* Registration inside a custom capability must capture script C even though
 * the neutral job has no entry. Later callbacks use associated B in document D. */
static unsigned neutral_dom_probes, neutral_dom_callbacks;
static JSValue neutral_dom_probe(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv)
{
    (void)self;(void)argc;(void)argv;os64_js_outcome_t out;
    check(!os64_js_settings_realm(ce_runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_ENTRY,&out)&&
          out.status==OS64_JS_ADMISSION_REFUSED,"neutral DOM: custom capability has no entry before or after nested reactions");
    check(ctx==ce_contexts[2]&&
          os64_js_settings_realm(ce_runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_CURRENT,&out)==ce_realms[2]&&
          os64_js_settings_realm(ce_runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_INCUMBENT,&out)==ce_realms[2],
          "neutral DOM: executing capability supplies current and incumbent C");
    neutral_dom_probes++;
    return JS_UNDEFINED;
}
static JSValue neutral_dom_callback(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv,int magic,JSValue *data)
{
    (void)self;(void)argc;(void)argv;(void)magic;(void)data;
    ce_settings_check(ctx,true);neutral_dom_callbacks++;
    return JS_UNDEFINED;
}
static void dom_neutral_promise_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++)
    for(unsigned reject=0;reject<2;reject++) {
        CeFixture f;if(!ce_open(&f,policy,5)){ce_close(&f);continue;}
        ce_incumbent=2;neutral_dom_probes=neutral_dom_callbacks=0;
        JSContext *ctx=ce_contexts[2];JSValue global=JS_GetGlobalObject(ctx);
        check(JS_SetPropertyStr(ctx,global,"neutralProbe",JS_NewCFunction(ctx,neutral_dom_probe,"neutralProbe",0))>=0&&
              JS_SetPropertyStr(ctx,global,"neutralCallback",JS_NewCFunctionData(ctx,neutral_dom_callback,0,0,0,NULL))>=0,
              "neutral DOM: native settings probes installed");
        JS_FreeValue(ctx,global);
        realm_script(ce_realms[1],
            "globalThis.listener=new Proxy(C.neutralCallback,{});"
            "globalThis.Species=function(executor){executor(C.cap,C.cap)};",
            "neutral DOM: species and callback association belong to B");
        realm_script(ce_realms[3],"document.body.innerHTML='<x-probe></x-probe>';",
                     "neutral DOM: upgrade candidate belongs to D");
        realm_script(ce_realms[2],
            "globalThis.cap=value=>{neutralProbe();"
            "D.customElements.define('x-probe',B.ctor);neutralProbe();"
            "D.document.body.addEventListener('neutral',B.listener);"
            "globalThis.mo=new D.MutationObserver(B.listener);mo.observe(D.document.body,{attributes:true});"
            "neutralProbe()};",
            "neutral DOM: capability registers custom elements listeners and observers");
        realm_script(ce_realms[0],
            "globalThis.pending=new Promise((r,j)=>{E.resolve=r;E.reject=j});"
            "pending.constructor={[Symbol.species]:B.Species};pending.then(null,null);",
            "neutral DOM: creator A registers absent handlers");
        realm_script(ce_realms[4],reject?"reject(73)":"resolve(73)","neutral DOM: independent E settlement");
        check(neutral_dom_probes==3&&ce_constructors==1&&ce_calls[0]==1,
              "neutral DOM: synchronous upgrades nest and restore the neutral scope");
        realm_script(ce_realms[4],
            "D.document.body.dispatchEvent(new D.Event('neutral'));D.document.body.setAttribute('data-neutral','delivered');",
            "neutral DOM: E triggers later deliveries in D");
        os64_js_outcome_t out;(void)os64_dom_reactions_run(f.bindings[4],&out);
        check(neutral_dom_callbacks==2,"neutral DOM: listener and observer retain C capture through native B callbacks");
        realm_script(ce_realms[2],"mo.disconnect()","neutral DOM: observer released");
        check(!JS_GetEntryContext(JS_GetRuntime(ctx))&&!JS_GetCurrentContext(JS_GetRuntime(ctx))&&
              !JS_GetIncumbentContext(JS_GetRuntime(ctx)),"neutral DOM: job and callback scopes drained");
        ce_close(&f);
    }
}
