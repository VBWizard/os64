/* A assigns, B creates the callback proxy, C supplies native code, D owns
 * the target. Inline handlers instead capture their document explicitly. */
static os64_js_runtime_t *handler_runtime;
static os64_js_realm_t *handler_realms[4];
static JSContext *handler_contexts[4];
static unsigned handler_calls, handler_entry, handler_incumbent, handler_family;
static bool handler_clear;
static JSValue handler_target;

static void handler_settings_check(void)
{
    os64_js_outcome_t out;
    check(os64_js_settings_realm(handler_runtime, OS64_JS_ABI_ID, OS64_JS_SETTINGS_ENTRY, &out)
          == handler_realms[handler_entry], "handler settings: associated entry independent of code");
    check(os64_js_settings_realm(handler_runtime, OS64_JS_ABI_ID, OS64_JS_SETTINGS_INCUMBENT, &out)
          == handler_realms[handler_incumbent], "handler settings: captured incumbent independent of dispatcher");
    check(os64_js_settings_realm(handler_runtime, OS64_JS_ABI_ID, OS64_JS_SETTINGS_CURRENT, &out)
          == handler_realms[2], "handler settings: executable current is C");
}

static JSValue handler_probe(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                             int magic, JSValue *data)
{
    (void)magic; (void)data;
    handler_settings_check();
    check(ctx == handler_contexts[handler_entry], "handler settings: native data incoming follows entry");
    check(JS_IsObject(self) && JS_VALUE_GET_PTR(self) == JS_VALUE_GET_PTR(handler_target),
          "handler settings: receiver is current target");
    check(argc == (handler_family == 3 ? 5 : 1), "handler settings: ordinary and onerror arguments");
    if (handler_family == 3) {
        const char *message = JS_ToCString(ctx, argv[0]);
        check(message && !strcmp(message,"boom") && JS_IsNumber(argv[2]) && JS_IsNumber(argv[3]),
              "handler settings: onerror receives error details");
        JS_FreeCString(ctx, message);
    }
    handler_calls++;
    if (handler_clear) {
        handler_clear = false;
        const char *property = handler_family == 2 ? "onabort" : handler_family == 3 ? "onerror" : "onclick";
        if (JS_SetPropertyStr(ctx, handler_target, property, JS_NULL) < 0) return JS_EXCEPTION;
        handler_settings_check();
    }
    return JS_NewBool(ctx, handler_family == 3);
}

static JSValue handler_inspect(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)ctx; (void)self; (void)argc; (void)argv;
    handler_settings_check(); handler_calls++; return JS_UNDEFINED;
}

static JSValue handler_arm(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)ctx; (void)self; (void)argc; (void)argv;
    dom_fail_at = dom_attempts + 1; return JS_UNDEFINED;
}

static void dom_handler_settings_cases(void)
{
    for (int policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; policy++)
    for (unsigned family = 0; family < 4; family++)
    for (unsigned mode = 0; mode < 18; mode++) {
        if (((mode == 12 || mode == 13 || mode == 15 || mode == 16) && family != 1) ||
            (mode == 14 && family != 0) || (mode == 17 && family != 2)) continue;
        p2b_ms=1000; p2b_us=1000000;
        handler_calls=0; handler_family=family; handler_entry=1; handler_incumbent=0; handler_clear=false;
        handler_target=JS_UNDEFINED;
        os64_js_config_t config=fixture_config(); config.limits.memory_bytes=64u<<20;
        os64_js_runtime_t *runtime=NULL; os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config,policy,OS64_JS_ABI_ID,&runtime,&out)==OS64_JS_OK,
              "handler settings: runtime created");
        if(!runtime)continue;
        handler_runtime=runtime;
        os64_dom_cluster_limits_t limits={64u<<20,16u<<20,8u<<20,256u<<20,128u<<20};
        os64_dom_cluster_t *cluster=os64_dom_cluster_create_with_handler_settings(runtime,&limits,&out);
        check(cluster!=NULL,"handler settings: opt-in cluster created");
        if(!cluster){os64_js_destroy(runtime);continue;}
        os64_dom_t *bindings[4]={0}; os64_html_document_t *document_d=NULL; bool ready=true;
        handler_realms[0]=os64_js_primary_realm(runtime,OS64_JS_ABI_ID,&out);
        for(int i=0;i<4;i++) {
            if(i)check(os64_js_realm_create(runtime,OS64_JS_ABI_ID,&handler_realms[i],&out)==OS64_JS_OK,"handler settings: child created");
            handler_contexts[i]=os64_js_realm_context(handler_realms[i],OS64_JS_ABI_ID,&out);
            DomFixture f=fixture_configured(0,0,&config);os64_js_destroy(f.runtime);
            os64_dom_options_t options=p2b_options("https://handlers.example/");
            os64_dom_admission_t a=os64_dom_create_in_realm(cluster,handler_realms[i],f.document,f.state,&options,&out);
            bindings[i]=a.binding;ready &= a.binding!=NULL;
            if(i==3)document_d=f.document;
            check(a.binding&&a.consumed,"handler settings: binding installed");
            if(!a.consumed){os64_page_state_free(f.state);os64_html_document_free(f.document);}
        }
        if(ready) {
            for(int i=0;i<4;i++) {
                JSContext *ctx=handler_contexts[i]; JSValue global=JS_GetGlobalObject(ctx);
                for(int j=0;j<4;j++) {
                    char name[]={(char)('A'+j),0};
                    check(JS_SetPropertyStr(ctx,global,name,JS_GetGlobalObject(handler_contexts[j]))>=0,"handler settings: trusted realm exchange");
                }
                if(i==2) {
                    check(JS_SetPropertyStr(ctx,global,"probe",JS_NewCFunctionData(ctx,handler_probe,1,0,0,NULL))>=0,"handler settings: native callback exported");
                    check(JS_SetPropertyStr(ctx,global,"inspect",JS_NewCFunction(ctx,handler_inspect,"inspect",0))>=0,"handler settings: inline probe exported");
                    check(JS_SetPropertyStr(ctx,global,"arm",JS_NewCFunction(ctx,handler_arm,"arm",0))>=0,"handler settings: allocation seam exported");
                }
                JS_FreeValue(ctx,global);
                realm_script(handler_realms[i],STORAGE_ASSERT "globalThis.errors=[];globalThis.errorLedger=e=>errors.push(e.error);addEventListener('error',errorLedger);",
                             "handler settings: error ledgers");
            }
            realm_script(handler_realms[1],"globalThis.p=new Proxy(C.probe,{});","handler settings: associated proxy created");
            const char *property=family==2?"onabort":family==3?"onerror":"onclick";
            const char *type=family==2?"abort":family==3?"error":"click";
            realm_script(handler_realms[3],family==1?"globalThis.target=document.getElementById('one');":
                         family==2?"globalThis.target=new AbortController().signal;":"globalThis.target=window;","handler settings: target created");
            JSValue global=JS_GetGlobalObject(handler_contexts[3]);
            handler_target=JS_GetPropertyStr(handler_contexts[3],global,"target");JS_FreeValue(handler_contexts[3],global);
            char assign[160],dispatch[300];
            snprintf(assign,sizeof(assign),"D.target.%s=B.p;assert(D.target.%s===B.p);",property,property);
            snprintf(dispatch,sizeof(dispatch),"D.target.dispatchEvent(new D.%s('%s',{cancelable:true,message:'boom',filename:'source',lineno:7,colno:9,error:42}));",
                     family==3?"ErrorEvent":"Event",type);
            if(mode==12) {
                JSContext *ctx=handler_contexts[0];
                JSValue sentinel=JS_NewObject(ctx), object=JS_NewObject(ctx);
                JSValue cglobal=JS_GetGlobalObject(handler_contexts[2]);
                JSValue native=JS_GetPropertyStr(ctx,cglobal,"probe");
                JS_Throw(ctx,JS_DupValue(ctx,sentinel));
                check(!JS_ClearFunctionScriptProvenance(JS_UNDEFINED,"",0)&&!JS_ClearFunctionScriptProvenance(object,"",0)&&
                      !JS_ClearFunctionScriptProvenance(native,"",0),"handler settings: provenance operation refuses non-bytecode values");
                JSValue pending=JS_GetException(ctx);
                check(JS_VALUE_GET_PTR(pending)==JS_VALUE_GET_PTR(sentinel),"handler settings: provenance refusal preserves exception identity");
                JS_FreeValue(ctx,pending);JS_FreeValue(ctx,sentinel);JS_FreeValue(ctx,object);JS_FreeValue(ctx,native);JS_FreeValue(ctx,cglobal);
                realm_script(handler_realms[0],"D.target.setAttribute('onclick','C.inspect(); return false');globalThis.inline=D.target.onclick;",
                             "handler settings: foreign getter compiles document attribute");
                handler_entry=handler_incumbent=3;
                realm_script(handler_realms[0],dispatch,"handler settings: inline dispatch uses document settings");
                handler_entry=handler_incumbent=0;
                realm_script(handler_realms[0],"inline();","handler settings: scriptless inline closure preserves direct caller incumbent");
                handler_entry=handler_incumbent=3;
                realm_script(handler_realms[0],"D.target.setAttribute('onclick','C.inspect()');D.target.dispatchEvent(new D.Event('click'));",
                             "handler settings: attribute refresh recaptures document settings");
                check(handler_calls==3,"handler settings: inline compilation and direct invocation");
                realm_script(handler_realms[2],"globalThis.victim=function(){C.inspect()};","handler settings: existing script-bearing closure");
                realm_script(handler_realms[0],"D.target.setAttribute('onclick','}, true ? C.victim : function() {');assert(D.target.onclick===null);",
                             "handler settings: escaped body cannot alter returned closure provenance");
                handler_entry=0;handler_incumbent=2;
                realm_script(handler_realms[0],"C.victim();","handler settings: refused alias retains its original provenance");
                check(handler_calls==4,"handler settings: escaped body never dispatched");
            } else if(mode==13) {
                realm_script(handler_realms[0],"D.target.setAttribute('onclick','C.inspect()');B.document.adoptNode(D.target);globalThis.inline=D.target.onclick;",
                             "handler settings: uncompiled attribute follows adopted document");
                handler_entry=handler_incumbent=1;
                realm_script(handler_realms[0],dispatch,"handler settings: adopted inline captures B");
                realm_script(handler_realms[0],"D.document.adoptNode(D.target);", "handler settings: compiled attribute adopted back");
                realm_script(handler_realms[0],dispatch,"handler settings: compiled inline keeps captured settings");
                check(handler_calls==2,"handler settings: adoption distinguishes raw and compiled attribute");
            } else if(mode==14) {
                realm_script(handler_realms[0],"D.document.body.onload=B.p;assert(D.onload===B.p);",
                             "handler settings: body reflects window handler capture");
                realm_script(handler_realms[2],"D.dispatchEvent(new D.Event('load'));","handler settings: reflected load prepares callback");
                check(handler_calls==1,"handler settings: reflected handler delivered");
            } else if(mode==15) {
                realm_script(handler_realms[0],"D.target.setAttribute('onclick','C.inspect();return false');",
                             "handler settings: raw attribute for host first use");
                handler_entry=handler_incumbent=3;
                os64_dom_event_t event={.type="click",.cancelable=true};bool prevented=false;
                check(os64_dom_dispatch(bindings[3],wa_by_id(document_d->document,"one"),&event,&prevented,&out)==OS64_JS_OK&&prevented&&handler_calls==1,
                      "handler settings: host first compile captures document");
            } else if(mode==16) {
                realm_script(handler_realms[0],"D.target.setAttribute('onclick','}');assert(D.target.onclick===null);",
                             "handler settings: malformed attribute reports and reads null");
                realm_script(handler_realms[3],"assert(errors.length===1);","handler settings: syntax report belongs to document");
                handler_entry=handler_incumbent=3;
                realm_script(handler_realms[0],"D.target.setAttribute('onclick','C.inspect()');D.target.dispatchEvent(new D.Event('click'));",
                             "handler settings: corrected attribute recompiles");
                check(handler_calls==1,"handler settings: compilation failure does not poison slot");
            } else if(mode==17) {
                realm_script(handler_realms[1],"p=((target)=>function(){return target})(D.target);D.target.onabort=p;A.weak=new WeakRef(D.target);D.target=null;p=null;",
                             "handler settings: plain target and captured closure cycle");
                JS_FreeValue(handler_contexts[3],handler_target);handler_target=JS_UNDEFINED;
                JS_RunGC(JS_GetRuntime(handler_contexts[0]));
                realm_script(handler_realms[0],"assert(weak.deref()===undefined);","handler settings: engine-held handler cycle collected");
            } else if(mode==10) {
                char code[260];snprintf(code,sizeof(code),"C.arm();try{%s}catch(e){if(e.name!=='QuotaExceededError')throw e}",assign);
                realm_script(handler_realms[0],code,"handler settings: native slot allocation refusal");dom_fail_at=0;
                realm_script(handler_realms[0],dispatch,"handler settings: refused slot never executes");
                check(handler_calls==0,"handler settings: refused capture released");
            } else {
                realm_script(handler_realms[0],assign,"handler settings: property captures assignment");
                if(mode>=1&&mode<=4) {
                    check(os64_js_realm_retire(handler_realms[mode-1],OS64_JS_ABI_ID,&out)==OS64_JS_OK,"handler settings: independent role retired");
                    realm_script(handler_realms[mode==1?2:0],dispatch,"handler settings: retired callback dispatch");
                    check(handler_calls==0,"handler settings: retired role prevents author call");
                } else if(mode==5) {
                    char code[200];snprintf(code,sizeof(code),"D.target.%s=C.probe;",property);
                    JSValue value=JS_Eval(handler_contexts[0],code,strlen(code),"unprepared-handler",JS_EVAL_TYPE_GLOBAL);
                    check(JS_IsException(value),"handler settings: unprepared assignment refused");
                    JS_FreeValue(handler_contexts[0],value);JS_FreeValue(handler_contexts[0],JS_GetException(handler_contexts[0]));
                    realm_script(handler_realms[2],dispatch,"handler settings: refusal preserves original capture");
                    check(handler_calls==1,"handler settings: original handler survives refusal");
                    char promise[450];snprintf(promise,sizeof(promise),"globalThis.refused=false;Promise.resolve().then(()=>{try{%s}catch(e){refused=e.name==='InvalidStateError'}})",code);
                    realm_script(handler_realms[0],promise,"handler settings: Promise assignment remains unprepared");
                    realm_script(handler_realms[0],"assert(refused);","handler settings: no implicit Promise capture");
                } else if(mode==6) {
                    char code[1024];snprintf(code,sizeof(code),"B.object={get handleEvent(){throw Error('must not look up')}};D.target.%s=B.object;assert(D.target.%s===B.object);%s",property,property,dispatch);
                    realm_script(handler_realms[0],code,"handler settings: noncallable object retained without handleEvent lookup");
                    realm_script(handler_realms[3],family==3?"assert(errors.length===1&&errors[0]===42);":"assert(errors.length===0);","handler settings: noncallable invocation is a no-op");
                    check(handler_calls==0,"handler settings: noncallable object does not call method");
                } else if(mode==7) {
                    handler_clear=true;
                    realm_script(handler_realms[2],dispatch,"handler settings: callback clears own slot");
                    realm_script(handler_realms[2],dispatch,"handler settings: cleared slot stays empty");
                    check(handler_calls==1,"handler settings: in-flight capture survives clearing");
                } else if(mode==8) {
                    realm_script(handler_realms[1],"globalThis.rev=Proxy.revocable(C.probe,{});p=rev.proxy;","handler settings: revocable callback");
                    realm_script(handler_realms[0],assign,"handler settings: revocable callback captured");
                    realm_script(handler_realms[1],"rev.revoke();","handler settings: callback revoked");
                    realm_script(handler_realms[0],dispatch,"handler settings: revoked callback error reported");
                    realm_script(handler_realms[2],"assert(errors.length===1&&errors[0] instanceof B.TypeError);","handler settings: revoked proxy error belongs to associated realm");
                } else if(mode==9) {
                    realm_script(handler_realms[1],"p=function(){while(true){}};","handler settings: interruptible callback");
                    realm_script(handler_realms[0],assign,"handler settings: interruptible handler assigned");
                    char code[420];snprintf(code,sizeof(code),"try{%s}catch(e){globalThis.caught=new ArrayBuffer(8<<20)}",dispatch);
                    size_t before=large_asks;dom_clock_step=10000;
                    os64_js_realm_run(handler_realms[0],code,strlen(code),"handler-interrupt",&out);dom_clock_step=0;
                    check(out.status==OS64_JS_LIMIT&&large_asks==before,"handler settings: terminal failure cannot enter caller catch");
                } else if(mode==11) {
                    char code[240];snprintf(code,sizeof(code),"globalThis.weak=new WeakRef(B.p);D.target.%s=null;B.p=null;",property);
                    realm_script(handler_realms[0],code,"handler settings: cleared proxy reference");
                    JS_RunGC(JS_GetRuntime(handler_contexts[0]));
                    realm_script(handler_realms[0],"assert(weak.deref()===undefined);","handler settings: clearing releases callback edge");
                } else {
                    if(family==3)
                        realm_script(handler_realms[2],"assert(!D.target.dispatchEvent(new D.ErrorEvent('error',{cancelable:true,message:'boom',error:42})));",
                                     "handler settings: window onerror true return cancels");
                    else realm_script(handler_realms[2],dispatch,"handler settings: dispatcher differs from captured caller");
                    if(family==0||family==1) {
                        os64_dom_event_t event={.type=type,.cancelable=true};bool prevented=false;
                        check(os64_dom_dispatch(bindings[3],family==1?wa_by_id(document_d->document,"one"):NULL,&event,&prevented,&out)==OS64_JS_OK&&prevented,
                              "handler settings: host dispatch prepares and honors false return");
                    }
                    handler_incumbent=2;
                    realm_script(handler_realms[2],assign,"handler settings: same callback reassignment replaces incumbent");
                    realm_script(handler_realms[0],dispatch,"handler settings: replacement capture used");
                    check(handler_calls==(family<2?3u:2u),"handler settings: host and script calls delivered");
                }
            }
            check(JS_GetEntryContext(JS_GetRuntime(handler_contexts[0]))==NULL&&JS_GetIncumbentContext(JS_GetRuntime(handler_contexts[0]))==NULL,
                  "handler settings: settings restored after host return");
            for(int i=0;i<4;i++)memory_balanced(bindings[i],"handler settings before drain");
        }
        JS_FreeValueRT(JS_GetRuntime(handler_contexts[0]),handler_target);
        os64_dom_cluster_drain(cluster);os64_js_destroy(runtime);os64_dom_cluster_free(cluster);
        check(live==0&&dom_live==0&&!dom_allocations,"handler settings: both teardown policies release ownership");
    }
}
