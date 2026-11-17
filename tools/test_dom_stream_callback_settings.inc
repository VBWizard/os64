/* A captures, B owns the proxy callback, C executes native probes, D owns
 * the self-hosted implementation/data, and E invokes the retained stream. */
static JSValue sc_property(JSContext *ctx,const char *name)
{
    JSValue global=JS_GetGlobalObject(ctx),value=JS_GetPropertyStr(ctx,global,name);
    JS_FreeValue(ctx,global);return value;
}
static unsigned sc_calls[6], sc_family;
static bool sc_throw, sc_cancel;
static JSValue sc_probe(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv,int phase,JSValue *data)
{
    (void)data;ce_settings_check(ctx,true);sc_calls[phase]++;
    if(sc_cancel){sc_cancel=false;os64_js_cancel(ce_runtime);return JS_UNDEFINED;}
    if(sc_throw&&(phase==0||phase==1||phase==2)){sc_throw=false;return JS_Throw(ctx,sc_property(ce_contexts[2],"error"));}
    if(phase==4)return JS_NewInt32(ctx,1);
    if(phase==5)return JS_UNDEFINED;
    if(phase==3) {
        check(argc==1&&JS_IsUndefined(self),"stream callbacks: size receives chunk and undefined receiver");
        return sc_property(ce_contexts[2],"number");
    }
    check(JS_IsObject(self),"stream callbacks: underlying dictionary is callback receiver");
    if(phase==0)check(argc==1&&JS_IsObject(argv[0]),"stream callbacks: start receives controller");
    if(phase==1) {
        if(sc_family<2) {
            check(argc==1&&JS_IsObject(argv[0]),"stream callbacks: pull receives controller");
            JSValue result=ce_method(ctx,argv[0],"close",0,NULL);if(JS_IsException(result))return result;JS_FreeValue(ctx,result);
            if(sc_family==1){JSValue request=JS_GetPropertyStr(ctx,argv[0],"byobRequest"),zero=JS_NewInt32(ctx,0);result=ce_method(ctx,request,"respond",1,&zero);JS_FreeValue(ctx,request);if(JS_IsException(result))return result;JS_FreeValue(ctx,result);}
        } else {
            check(argc==2&&JS_IsObject(argv[1])&&JS_IsNumber(argv[0]),"stream callbacks: write/transform receive chunk and controller");
            if(sc_family==3){JSValue result=ce_method(ctx,argv[1],"enqueue",1,argv);if(JS_IsException(result))return result;JS_FreeValue(ctx,result);}
        }
    }
    return phase?sc_property(ce_contexts[2],"thenable"):JS_UNDEFINED;
}
static bool sc_open(CeFixture *f,int policy,unsigned family,bool enabled)
{
    if(!ce_open(f,policy,enabled?7:6))return false;
    memset(sc_calls,0,sizeof(sc_calls));sc_family=family;sc_throw=sc_cancel=false;
    JSContext *ctx=ce_contexts[2];JSValue global=JS_GetGlobalObject(ctx);
    static const char *const names[]={"start","action","finish","size","convert","thenGet"};
    for(int i=0;i<6;i++)check(JS_SetPropertyStr(ctx,global,names[i],JS_NewCFunctionData(ctx,sc_probe,0,i,0,NULL))>=0,
          "stream callbacks: independent native probe installed");
    JS_FreeValue(ctx,global);
    realm_script(ce_realms[2],"globalThis.error=new TypeError('stream callback');globalThis.number={valueOf:convert};"
                 "globalThis.thenable={};Object.defineProperty(thenable,'then',{get:thenGet});",
                 "stream callbacks: return coercion objects belong to C");
    realm_script(ce_realms[1],"globalThis.start=new Proxy(C.start,{});globalThis.action=new Proxy(C.action,{});"
                 "globalThis.finish=new Proxy(C.finish,{});globalThis.size=new Proxy(C.size,{});"
                 "Object.setPrototypeOf(action,A.Function.prototype);",
                 "stream callbacks: associated B is independent of execution and prototypes");
    const char *sources[]={
        "globalThis.source={start:B.start,pull:B.action,cancel:B.finish};globalThis.strategy={highWaterMark:0,size:B.size}",
        "globalThis.source={type:'bytes',start:B.start,pull:B.action,cancel:B.finish};globalThis.strategy={highWaterMark:0}",
        "globalThis.source={start:B.start,write:B.action,close:B.finish,abort:B.finish};globalThis.strategy={highWaterMark:1,size:B.size}",
        "globalThis.source={start:B.start,transform:B.action,flush:B.finish,cancel:B.finish};globalThis.strategy={highWaterMark:1,size:B.size}"
    };
    realm_script(ce_realms[3],sources[family],"stream callbacks: source/sink/transformer and strategy belong to D");
    return true;
}
static void sc_construct(unsigned family)
{
    const char *sources[]={"globalThis.stream=new D.ReadableStream(D.source,D.strategy)",
        "globalThis.stream=new D.ReadableStream(D.source,D.strategy)",
        "globalThis.stream=new D.WritableStream(D.source,D.strategy)",
        "globalThis.stream=new D.TransformStream(D.source,D.strategy)"};
    realm_script(ce_realms[0],sources[family],"stream callbacks: construction captures A through implementation D");
}
static const char *sc_operation(unsigned family,bool finish)
{
    static const char *const action[]={
        "A.stream.getReader().read()","A.stream.getReader({mode:'byob'}).read(new Uint8Array(1))",
        "A.stream.getWriter().write(42)",
        "(()=>{let r=A.stream.readable.getReader();r.read().catch(()=>{});return A.stream.writable.getWriter().write(42)})()"};
    static const char *const ending[]={"A.stream.cancel('reason')","A.stream.cancel('reason')",
        "A.stream.abort('reason')","A.stream.readable.cancel('reason')"};
    return finish?ending[family]:action[family];
}
static void dom_stream_callback_settings_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++) {
        for(unsigned family=0;family<4;family++)for(unsigned mode=0;mode<3;mode++) {
            CeFixture f;if(!sc_open(&f,policy,family,true)){ce_close(&f);continue;}
            sc_construct(family);check(sc_calls[0]==1,"stream callbacks: start delivered once under capture settings");
            if(mode==2)sc_throw=true;
            JS_RunGC(JS_GetRuntime(ce_contexts[0]));
            char source[768];snprintf(source,sizeof(source),"globalThis.done=0;globalThis.failure=undefined;%s.then(()=>done++,e=>failure=e)",sc_operation(family,mode==1));
            realm_script(ce_realms[4],source,"stream callbacks: retained stream invokes from E");
            realm_script(ce_realms[4],mode==2?"assert(failure===C.error&&done===0)":"assert(done===1&&failure===undefined)",
                         "stream callbacks: promise completion and thrown identity survive conversion");
            if(mode<2)check(sc_calls[5]>=1,"stream callbacks: then getter runs inside associated entry/captured incumbent");
            if(family>=2&&mode==0)check(sc_calls[3]==1&&sc_calls[4]==1,
                 "stream callbacks: size return conversion shares callback scope");
            check(!JS_GetEntryContext(JS_GetRuntime(ce_contexts[0]))&&!JS_GetIncumbentContext(JS_GetRuntime(ce_contexts[0])),
                  "stream callbacks: scopes unwind after callbacks and jobs");ce_close(&f);
        }
        for(unsigned role=0;role<4;role++) {
            CeFixture f;if(!sc_open(&f,policy,2,true)){ce_close(&f);continue;}
            sc_construct(2);os64_js_outcome_t out;
            check(os64_js_realm_retire(ce_realms[role],OS64_JS_ABI_ID,&out)==OS64_JS_OK,
                  "stream callbacks: independent role retires");
            realm_script(ce_realms[4],"globalThis.done=0;globalThis.failure=undefined;A.stream.abort().then(()=>done++,e=>failure=e)",
                         "stream callbacks: retained data still schedules checked callback");
            realm_script(ce_realms[4],role==3?"assert(done===1&&failure===undefined)":"assert(done===0&&failure.name==='InvalidStateError')",
                         "stream callbacks: callback roles gate execution, data-owner departure does not");
            check(sc_calls[2]==(unsigned)(role==3),"stream callbacks: ineligible callback does not execute");ce_close(&f);
        }
        for(unsigned mode=0;mode<2;mode++) {
            CeFixture f;if(!sc_open(&f,policy,0,true)){ce_close(&f);continue;}
            if(mode==0) {
                sc_throw=true;
                realm_script(ce_realms[0],"let caught=false;try{new D.ReadableStream(D.source,D.strategy)}catch(e){caught=e===C.error}assert(caught)",
                             "stream callbacks: start any-return throws synchronously");
            } else {
                sc_cancel=true;os64_js_outcome_t out;const char *source="new D.ReadableStream(D.source,D.strategy);globalThis.after=true";
                check(os64_js_realm_run(ce_realms[0],source,strlen(source),"stream-cancel",&out)==OS64_JS_CANCELLED,
                      "stream callbacks: terminal cancellation cannot become rejection");
                JSValue after=sc_property(ce_contexts[0],"after");check(JS_IsUndefined(after),"stream callbacks: cancellation cannot resume caller");JS_FreeValue(ce_contexts[0],after);
            }
            ce_close(&f);
        }
        CeFixture f;if(!sc_open(&f,policy,0,true)){ce_close(&f);continue;}
        JSContext *ctx=ce_contexts[0];const char *raw="new D.ReadableStream({cancel:B.finish},{highWaterMark:0})";
        JSValue result=JS_Eval(ctx,raw,strlen(raw),"raw-stream",JS_EVAL_TYPE_GLOBAL);
        check(JS_IsException(result),"stream callbacks: unprepared raw conversion refuses instead of using implementation realm");
        if(JS_IsException(result))JS_FreeValue(ctx,JS_GetException(ctx));
        JS_FreeValue(ctx,result);ce_close(&f);
    }
}

static void dom_stream_callback_failure_cases(void)
{
    /* Both capture and invocation allocate engine-traced holders/promises.
     * Compile outside the seam; sweep the whole operation, then root teardown. */
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++)
    for(unsigned family=0;family<3;family++) {
        size_t inventory=0;
        for(size_t cut=0;cut<=inventory;cut++) {
            CeFixture f;if(!sc_open(&f,policy,2,true)){ce_close(&f);continue;}
            realm_script(ce_realms[1],
                "globalThis.safe=()=>({then(resolve){resolve()}});globalThis.size=()=>({valueOf(){return 1}})",
                "stream failure: callbacks compiled outside allocation seam");
            realm_script(ce_realms[0],
                "globalThis.stream=new D.WritableStream({write:B.safe,abort:B.safe},{size:B.size})",
                "stream failure: reusable stream captured");
            const char *operations[]={
                "globalThis.run=()=>{C.arm();return new D.WritableStream({start:B.safe,write:B.safe,abort:B.safe},{size:B.size})}",
                "globalThis.run=()=>{C.arm();return stream.getWriter().write(42)}",
                "globalThis.run=()=>{C.arm();return stream.abort('reason')}"};
            realm_script(ce_realms[0],operations[family],"stream failure: operation compiled outside seam");
            ce_cut=cut;ce_engine_cut=true;
            JSContext *ctx=ce_contexts[0];JSValue fn=sc_property(ctx,"run");os64_js_outcome_t out;
            check(os64_js_realm_task_begin(ce_realms[0],OS64_JS_ABI_ID,"stream-cut",&out)==OS64_JS_OK,
                  "stream failure: operation task opened");
            os64_js_status_t result=os64_js_realm_call(ce_realms[0],OS64_JS_ABI_ID,fn,JS_UNDEFINED,0,NULL,NULL,&out);
            size_t measured=attempts-ce_started;fail_at=0;JS_FreeValue(ctx,fn);
            if(!cut) {
                check(result==OS64_JS_OK&&measured<512,"stream failure: bounded allocation inventory measured");
                inventory=measured<512?measured:0;
            } else check(measured>=cut&&result!=OS64_JS_OK,
                         "stream failure: partial capture or invocation refuses after allocation failure");
            (void)os64_js_task_end(ce_runtime,OS64_JS_ABI_ID,&out);
            check(!JS_GetEntryContext(JS_GetRuntime(ctx))&&!JS_GetIncumbentContext(JS_GetRuntime(ctx)),
                  "stream failure: scopes unwind after partial work");
            ce_cut=0;JS_RunGC(JS_GetRuntime(ctx));ce_close(&f);
        }
        fprintf(stderr,"stream callback allocations: policy=%d family=%u cuts=%zu\n",policy,family,inventory);
    }
}
