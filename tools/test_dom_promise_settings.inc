/* A native Promise handler has no script frame to hide the captured backup.
 * Its nested WebIDL conversions must retain A, independently of entry C. */
static JSValue promise_dom_define(JSContext *ctx, JSValueConst self, int argc,
                                 JSValueConst *argv, int magic, JSValue *data)
{
    (void)self;(void)argc;(void)argv;(void)magic;(void)data;
    os64_js_outcome_t out;
    check(ctx==ce_contexts[2]&&os64_js_settings_realm(ce_runtime,OS64_JS_ABI_ID,
          OS64_JS_SETTINGS_ENTRY,&out)==ce_realms[2],"Promise DOM: executable C supplies job entry");
    check(os64_js_settings_realm(ce_runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_INCUMBENT,&out)==ce_realms[0],
          "Promise DOM: registration A supplies native incumbent");
    JSValue global=JS_GetGlobalObject(ce_contexts[3]);
    JSValue registry=JS_GetPropertyStr(ctx,global,"customElements");JS_FreeValue(ctx,global);
    global=JS_GetGlobalObject(ce_contexts[1]);
    JSValue args[2]={JS_NewString(ctx,"x-probe"),JS_GetPropertyStr(ctx,global,"ctor")};JS_FreeValue(ctx,global);
    JSValue result=ce_method(ctx,registry,"define",2,args);
    JS_FreeValue(ctx,registry);JS_FreeValue(ctx,args[0]);JS_FreeValue(ctx,args[1]);
    return result;
}

static void dom_promise_settings_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++) {
        CeFixture f;if(!ce_open(&f,policy,2)){ce_close(&f);continue;}
        JSValue global=JS_GetGlobalObject(ce_contexts[2]);
        check(JS_SetPropertyStr(ce_contexts[2],global,"promiseDefine",
              JS_NewCFunctionData(ce_contexts[2],promise_dom_define,1,0,0,NULL))>=0,"Promise DOM: native registration installed");
        JS_FreeValue(ce_contexts[2],global);
        realm_script(ce_realms[1],"globalThis.promiseHandler=new Proxy(C.promiseDefine,{});","Promise DOM: handler proxy B");
        realm_script(ce_realms[0],"Promise.resolve().then(B.promiseHandler);","Promise DOM: job defines custom element");
        realm_script(ce_realms[3],"globalThis.target=document.createElement('x-probe');document.body.appendChild(target);target.setAttribute('data-x','promise');",
                     "Promise DOM: definition delivers after Promise scope has ended");
        check(ce_constructors==1&&ce_calls[0]==1&&ce_calls[3]==1,"Promise DOM: constructor and reactions keep nested capture A");
        realm_script(ce_realms[0],
            "globalThis.events=0;globalThis.mutations=0;globalThis.streamValue=0;"
            "Promise.resolve().then(()=>{"
            "D.target.addEventListener('promise-event',()=>events++);"
            "globalThis.mo=new MutationObserver(()=>mutations++);mo.observe(D.target,{attributes:true});"
            "let reader=new ReadableStream({start(c){c.enqueue(42);c.close()}}).getReader();"
            "return reader.read().then(r=>{streamValue=r.value});"
            "}).then(()=>{D.target.dispatchEvent(new D.Event('promise-event'));D.target.setAttribute('data-x','observer')});",
            "Promise DOM: listener observer Streams and chained jobs cooperate");
        os64_js_outcome_t out;
        (void)os64_dom_reactions_run(f.bindings[0],&out);
        realm_script(ce_realms[0],"assert(events===1&&mutations===1&&streamValue===42);mo.disconnect();",
                     "Promise DOM: registered callbacks and stream result delivered");
        check(JS_GetEntryContext(JS_GetRuntime(ce_contexts[0]))==NULL&&JS_GetIncumbentContext(JS_GetRuntime(ce_contexts[0]))==NULL,
              "Promise DOM: jobs leave no settings scopes behind");
        ce_close(&f);
    }
}
