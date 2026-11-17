/* Capture A, associated B, native execution C, data/tree D and later caller E
 * are distinct. Native callbacks expose the backup incumbent directly. */
static unsigned sync_calls[5];
static int sync_throw_phase, sync_retire, sync_retire_after_call;
static bool sync_interface, sync_register, sync_adopt, sync_cancel;
static int sync_reenter_phase;

static JSValue sync_callback(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv,int phase,JSValue *data)
{
    (void)data;ce_settings_check(ctx,true);sync_calls[phase]++;
    if(sync_reenter_phase==phase) {
        sync_reenter_phase=-1;
        JSValue g=JS_GetGlobalObject(ce_contexts[0]),walk=JS_GetPropertyStr(ctx,g,"walk");JS_FreeValue(ctx,g);
        JSValue r=ce_method(ctx,walk,"nextNode",0,NULL);JS_FreeValue(ctx,walk);
        check(JS_IsException(r),"synchronous settings: active flag blocks recursive traversal during lookup call and coercion");
        if(JS_IsException(r))JS_FreeValue(ctx,JS_GetException(ctx));
        JS_FreeValue(ctx,r);ce_settings_check(ctx,true);
    }
    if(sync_cancel) { sync_cancel=false;os64_js_cancel(ce_runtime);return JS_UNDEFINED; }
    if(sync_throw_phase==phase) {
        sync_throw_phase=-1;
        JSValue global=JS_GetGlobalObject(ce_contexts[2]);
        JSValue error=JS_GetPropertyStr(ctx,global,"error");JS_FreeValue(ctx,global);
        return JS_Throw(ctx,error);
    }
    if(phase==0) {
        check(argc==3&&JS_IsString(argv[0])&&JS_IsString(argv[1])&&JS_IsObject(argv[2]),
              "synchronous settings: pair callback receives value key and original object");
        JSValue tag=JS_GetPropertyStr(ctx,self,"tag");const char *text=JS_ToCString(ctx,tag);
        check(text&&!strcmp(text,"receiver"),"synchronous settings: explicit thisArg survives proxy invocation");
        JS_FreeCString(ctx,text);JS_FreeValue(ctx,tag);
        if(sync_register) {
            sync_register=false;
            JSValue g=JS_GetGlobalObject(ce_contexts[3]),doc=JS_GetPropertyStr(ctx,g,"document");JS_FreeValue(ctx,g);
            JSValue body=JS_GetPropertyStr(ctx,doc,"body");JS_FreeValue(ctx,doc);
            g=JS_GetGlobalObject(ce_contexts[1]);JSValue args[2]={JS_NewString(ctx,"sync"),JS_GetPropertyStr(ctx,g,"listener")};JS_FreeValue(ctx,g);
            JSValue r=ce_method(ctx,body,"addEventListener",2,args);
            JS_FreeValue(ctx,args[0]);JS_FreeValue(ctx,args[1]);JS_FreeValue(ctx,body);
            if(JS_IsException(r))return r;
            JS_FreeValue(ctx,r);ce_settings_check(ctx,true);
        }
        if(sync_retire_after_call>=0) {
            os64_js_outcome_t out;os64_js_realm_retire(ce_realms[sync_retire_after_call],OS64_JS_ABI_ID,&out);
            sync_retire_after_call=-1;
        }
        return JS_UNDEFINED;
    }
    if(phase==1) {
        check(argc==1&&JS_IsObject(argv[0]),"synchronous settings: filter receives its live node wrapper");
        check(sync_interface?JS_IsObject(self):JS_IsUndefined(self),"synchronous settings: interface versus callable receiver");
        JSValue global=JS_GetGlobalObject(ce_contexts[2]),value=JS_GetPropertyStr(ctx,global,"answer");JS_FreeValue(ctx,global);
        return value;
    }
    if(phase==2) {
        if(sync_adopt) {
            sync_adopt=false;
            JSValue g=JS_GetGlobalObject(ce_contexts[3]),root=JS_GetPropertyStr(ctx,g,"root");JS_FreeValue(ctx,g);
            g=JS_GetGlobalObject(ce_contexts[4]);JSValue doc=JS_GetPropertyStr(ctx,g,"document");JS_FreeValue(ctx,g);
            JSValue moved=ce_method(ctx,doc,"adoptNode",1,&root);JS_FreeValue(ctx,doc);JS_FreeValue(ctx,root);
            if(JS_IsException(moved))return moved;
            JS_FreeValue(ctx,moved);
        }
        if(sync_retire>=0) {
            os64_js_outcome_t out;os64_js_realm_retire(ce_realms[sync_retire],OS64_JS_ABI_ID,&out);
            sync_retire=-1;
        }
        JSValue global=JS_GetGlobalObject(ce_contexts[2]),value=JS_GetPropertyStr(ctx,global,"filterCall");JS_FreeValue(ctx,global);
        return value;
    }
    if(phase==3)return JS_NewInt32(ctx,65537); /* unsigned-short conversion yields FILTER_ACCEPT */
    check(argc==1,"synchronous settings: nested listener receives its event");
    return JS_UNDEFINED;
}

static bool sync_open(CeFixture *f,int policy,unsigned mode)
{
    if(!ce_open(f,policy,mode))return false;
    memset(sync_calls,0,sizeof(sync_calls));sync_throw_phase=sync_retire=sync_retire_after_call=-1;
    sync_interface=sync_register=sync_adopt=sync_cancel=false;sync_reenter_phase=-1;
    JSContext *ctx=ce_contexts[2];JSValue global=JS_GetGlobalObject(ctx);
    static const char *const names[]={"pairCall","filterCall","filterGet","convert","listener"};
    for(int i=0;i<5;i++)check(JS_SetPropertyStr(ctx,global,names[i],JS_NewCFunctionData(ctx,sync_callback,0,i,0,NULL))>=0,
                             "synchronous settings: native role probe installed");
    JS_FreeValue(ctx,global);
    realm_script(ce_realms[2],"globalThis.answer={valueOf:convert};globalThis.error=new TypeError('callback failure');",
                 "synchronous settings: return conversion and exception identity belong to C");
    realm_script(ce_realms[1],
        "globalThis.pair=new Proxy(C.pairCall,{});globalThis.filter=new Proxy(C.filterCall,{});"
        "globalThis.listener=new Proxy(C.listener,{});globalThis.filterObject={};"
        "Object.defineProperty(filterObject,'acceptNode',{get:C.filterGet,configurable:true});"
        "Object.setPrototypeOf(filterObject,A.Object.prototype);Object.setPrototypeOf(pair,A.Function.prototype);",
        "synchronous settings: associated B is independent of target and prototype");
    realm_script(ce_realms[3],
        "globalThis.root=document.createElement('section');root.innerHTML='<span></span><span></span>';document.body.appendChild(root);"
        "globalThis.items=[new Headers({a:'one',b:'two'}),new FormData(),new URLSearchParams('a=one&b=two')];"
        "items[1].append('a','one');items[1].append('b','two');",
        "synchronous settings: tree and pair containers belong to D");
    return true;
}

static void dom_synchronous_settings_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++) {
        for(unsigned mode=0;mode<16;mode++) {
            CeFixture f;if(!sync_open(&f,policy,6)){ce_close(&f);continue;}
            if(mode<4) {
                sync_interface=(mode&1)!=0;
                realm_script(ce_realms[0],mode>=2?
                    (sync_interface?"globalThis.walk=D.document.createNodeIterator(D.root,1,B.filterObject)":"globalThis.walk=D.document.createNodeIterator(D.root,1,B.filter)"):
                    (sync_interface?"globalThis.walk=D.document.createTreeWalker(D.root,1,B.filterObject)":"globalThis.walk=D.document.createTreeWalker(D.root,1,B.filter)"),
                    "synchronous settings: NodeFilter captures A once");
                JS_RunGC(JS_GetRuntime(ce_contexts[0]));
                realm_script(ce_realms[4],"assert(A.walk.filter===B.filter||A.walk.filter===B.filterObject);assert(A.walk.nextNode()!==null)",
                             "synchronous settings: later E traversal enters B with captured A");
                check(sync_calls[1]==1&&sync_calls[2]==(unsigned)sync_interface&&sync_calls[3]==1,
                      "synchronous settings: lookup call and numeric conversion share the scope");
            } else if(mode<7) {
                sync_interface=true;sync_throw_phase=(int)mode-3;
                realm_script(ce_realms[0],"globalThis.walk=D.document.createTreeWalker(D.root,1,B.filterObject)",
                             "synchronous settings: throwing filter captured");
                realm_script(ce_realms[4],
                    "let caught=false;try{A.walk.nextNode()}catch(e){caught=e===C.error}assert(caught);assert(A.walk.nextNode()!==null)",
                    "synchronous settings: callback getter or coercion throw keeps identity and clears active flag");
                check(sync_calls[3]>=1,"synchronous settings: traversal retries after abrupt completion");
            } else if(mode<11) {
                sync_interface=true;sync_retire=(int)mode-7;
                realm_script(ce_realms[0],"globalThis.walk=D.document.createTreeWalker(D.root,1,B.filterObject)",
                             "synchronous settings: getter retirement filter captured");
                realm_script(ce_realms[4],"let caught=false;try{A.walk.nextNode()}catch(e){caught=e.name==='InvalidStateError'}assert(caught)",
                             "synchronous settings: getter cannot enter a retired role");
                check(sync_calls[2]==1&&!sync_calls[1]&&!sync_calls[3],
                      "synchronous settings: eligibility rechecked after interface getter");
            } else if(mode==11) {
                realm_script(ce_realms[1],"Object.defineProperty(filterObject,'acceptNode',{value:17})",
                             "synchronous settings: noncallable interface method");
                realm_script(ce_realms[0],"globalThis.walk=D.document.createTreeWalker(D.root,1,B.filterObject)",
                             "synchronous settings: noncallable method defers failure until invocation");
                realm_script(ce_realms[4],"let caught=false;try{A.walk.nextNode()}catch(e){caught=e instanceof B.TypeError}assert(caught)",
                             "synchronous settings: missing method error belongs to associated B");
            }
            if(mode>=12) {
                sync_interface=true;
                if(mode==12){sync_adopt=true;sync_retire=3;}
                else sync_reenter_phase=mode==13?2:mode==14?1:3;
                realm_script(ce_realms[0],"globalThis.walk=D.document.createTreeWalker(D.root,1,B.filterObject)",
                             "synchronous settings: reentry filter captured");
                realm_script(ce_realms[4],"assert(A.walk.nextNode()!==null)",
                             "synchronous settings: reentry restores scope and candidate activity follows adoption");
                check(sync_calls[1]==1&&sync_calls[2]==1&&sync_calls[3]==1,
                      "synchronous settings: reentrant filter completes once");
            }
            check(!JS_GetEntryContext(JS_GetRuntime(ce_contexts[0]))&&!JS_GetIncumbentContext(JS_GetRuntime(ce_contexts[0])),
                  "synchronous settings: traversal scopes unwind");
            ce_close(&f);
        }
        for(unsigned family=0;family<3;family++)for(unsigned mode=0;mode<4;mode++) {
            CeFixture f;if(!sync_open(&f,policy,6)){ce_close(&f);continue;}
            char source[512];
            if(mode==0) {
                sync_register=true;
                snprintf(source,sizeof(source),"D.items[%u].forEach(B.pair,{tag:'receiver'})",family);
                realm_script(ce_realms[0],source,"synchronous settings: pair iterable captures A and invokes B");
                realm_script(ce_realms[4],"D.document.body.dispatchEvent(new D.Event('sync'))",
                             "synchronous settings: nested registration retains A after forEach returns");
                check(sync_calls[0]==2&&sync_calls[4]==1,"synchronous settings: pair iterations and nested listener delivered");
            } else if(mode==1) {
                sync_throw_phase=0;
                snprintf(source,sizeof(source),"let caught=false;try{D.items[%u].forEach(B.pair,{tag:'receiver'})}catch(e){caught=e===C.error}assert(caught)",family);
                realm_script(ce_realms[0],source,"synchronous settings: pair callback exception propagates unchanged");
                check(sync_calls[0]==1,"synchronous settings: pair exception stops iteration");
            } else if(mode==2) {
                sync_retire_after_call=0;
                snprintf(source,sizeof(source),"let caught=false;try{D.items[%u].forEach(B.pair,{tag:'receiver'})}catch(e){caught=e.name==='InvalidStateError'}assert(caught)",family);
                realm_script(ce_realms[0],source,"synchronous settings: next pair refuses retired incumbent");
                check(sync_calls[0]==1,"synchronous settings: retirement stops further callback entry");
            } else {
                /* Script callbacks supply their own active incumbent. This
                 * separate oracle checks live list semantics through mutation. */
                snprintf(source,sizeof(source),"let object=D.items[%u],seen=[];object.forEach((v,k,o)=>{assert(o===object);seen.push(k);if(k==='a'){o.delete('b');o.append('c','three')}});assert(seen.join(',')==='a,c')",family);
                realm_script(ce_realms[0],source,"synchronous settings: pair list is reread after callback mutation");
            }
            ce_close(&f);
        }
        for(unsigned filter=0;filter<2;filter++) {
            CeFixture f;if(!sync_open(&f,policy,6)){ce_close(&f);continue;}
            sync_cancel=true;
            const char *source=filter?"let w=D.document.createTreeWalker(D.root,1,B.filter);w.nextNode();globalThis.after=true":
                                      "D.items[0].forEach(B.pair,{tag:'receiver'});globalThis.after=true";
            os64_js_outcome_t out;
            check(os64_js_realm_run(ce_realms[0],source,strlen(source),"sync-cancel",&out)==OS64_JS_CANCELLED,
                  "synchronous settings: callback cancellation stays terminal");
            JSValue g=JS_GetGlobalObject(ce_contexts[0]),after=JS_GetPropertyStr(ce_contexts[0],g,"after");JS_FreeValue(ce_contexts[0],g);
            check(JS_IsUndefined(after),"synchronous settings: terminal callback cannot resume caller script");JS_FreeValue(ce_contexts[0],after);
            ce_close(&f);
        }
        for(unsigned enabled=0;enabled<2;enabled++) {
            CeFixture f;if(!sync_open(&f,policy,enabled?6:5)){ce_close(&f);continue;}
            const char *sources[]={"D.document.createTreeWalker(D.root,1,B.filter)","new D.Headers().forEach(B.pair)",
                                   "new D.FormData().forEach(B.pair)","new D.URLSearchParams().forEach(B.pair)"};
            for(unsigned i=0;i<4;i++) {
                JSContext *ctx=ce_contexts[0];JSValue r=JS_Eval(ctx,sources[i],strlen(sources[i]),"raw-sync",JS_EVAL_TYPE_GLOBAL);
                check(JS_IsException(r)==(bool)enabled,"synchronous settings: raw conversion refuses only under explicit opt-in, including empty iterables");
                if(JS_IsException(r))JS_FreeValue(ctx,JS_GetException(ctx));
                JS_FreeValue(ctx,r);
            }
            ce_close(&f);
        }
    }
}

static void dom_synchronous_failure_cases(void)
{
    /* The shared heap includes binding allocations as well as engine asks;
     * binding OOM may remain catchable while engine OOM poisons the task. */
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++)
    for(unsigned enabled=0;enabled<2;enabled++)
    for(unsigned heap=0;heap<2;heap++)for(unsigned family=0;family<(enabled?5u:1u);family++) {
        size_t inventory=0;
        for(size_t cut=0;cut<=inventory;cut++) {
            CeFixture f;if(!sync_open(&f,policy,enabled?6:5)){ce_close(&f);continue;}
            realm_script(ce_realms[1],"globalThis.safe=()=>{let scratch=new Array(4096);scratch.fill(1);return scratch[0]};globalThis.safeObject={get acceptNode(){return safe}}",
                         "synchronous failure: script callbacks compiled before injection");
            char source[320];
            if(family<2)snprintf(source,sizeof(source),"globalThis.run=()=>{C.arm();let w=D.document.%s(D.root,1,B.safeObject);return w.nextNode()}",
                                family?"createNodeIterator":"createTreeWalker");
            else snprintf(source,sizeof(source),"globalThis.run=()=>{C.arm();D.items[%u].forEach(B.safe)}",family-2);
            realm_script(ce_realms[0],source,"synchronous failure: operation compiled before injection");
            ce_cut=cut;ce_engine_cut=heap!=0;
            JSContext *ctx=ce_contexts[0];JSValue g=JS_GetGlobalObject(ctx),fn=JS_GetPropertyStr(ctx,g,"run");JS_FreeValue(ctx,g);
            os64_js_outcome_t out;
            check(os64_js_realm_task_begin(ce_realms[0],OS64_JS_ABI_ID,"sync-cut",&out)==OS64_JS_OK,
                  "synchronous failure: allocation task opened");
            os64_js_status_t result=os64_js_realm_call(ce_realms[0],OS64_JS_ABI_ID,fn,JS_UNDEFINED,0,NULL,NULL,&out);
            size_t measured=(heap?attempts:dom_attempts)-ce_started;dom_fail_at=fail_at=0;JS_FreeValue(ctx,fn);
            if(!cut) {
                check(result==OS64_JS_OK&&measured<256,"synchronous failure: bounded allocation inventory measured");
                inventory=measured<256?measured:0;
            } else check(measured>=cut&&result!=OS64_JS_OK,
                         "synchronous failure: partial capture traversal and invocation fail without success");
            (void)os64_js_task_end(ce_runtime,OS64_JS_ABI_ID,&out);
            check(!JS_GetEntryContext(JS_GetRuntime(ctx))&&!JS_GetIncumbentContext(JS_GetRuntime(ctx)),
                  "synchronous failure: callback scope unwound after allocation refusal");
            ce_cut=0;JS_RunGC(JS_GetRuntime(ctx));ce_close(&f);
        }
        fprintf(stderr,"synchronous allocations: policy=%d enabled=%u heap=%u family=%u cuts=%zu\n",policy,enabled,heap,family,inventory);
    }
}
