/* These paths compose the private continuation queue with checked author
 * callbacks. The implementation realm retires while live callers keep data. */
static unsigned flow_calls;
static JSValue flow_probe(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv,int magic,JSValue *data)
{
    (void)self;(void)argc;(void)argv;(void)magic;(void)data;
    ce_settings_check(ctx,true);flow_calls++;return JS_UNDEFINED;
}
static void dom_stream_continuation_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++)
    for(unsigned mode=0;mode<9;mode++) {
        CeFixture f;if(!ce_open(&f,policy,7)){ce_close(&f);continue;}
        unsigned family=mode==8?7:mode;
        ce_incumbent=1;flow_calls=0;
        JSValue global=JS_GetGlobalObject(ce_contexts[2]);
        check(JS_SetPropertyStr(ce_contexts[2],global,"flowProbe",JS_NewCFunctionData(ce_contexts[2],flow_probe,0,0,0,NULL))>=0,
              "stream continuations: native C checks entry B and actual script incumbent B");JS_FreeValue(ce_contexts[2],global);
        realm_script(ce_realms[1],
            "globalThis.error=new TypeError('stream stop');globalThis.writes=[];"
            "globalThis.source={start(c){C.flowProbe();c.enqueue(7);c.close()},cancel(){C.flowProbe()}};"
            "globalThis.bytes={type:'bytes',start(c){C.flowProbe();c.enqueue(new Uint8Array([7,8]));c.close()}};"
            "globalThis.sink={write(v){C.flowProbe();writes.push(v)},close(){C.flowProbe()},abort(){C.flowProbe()}};"
            "globalThis.pending={cancel(){C.flowProbe()}};globalThis.throwing={abort(){C.flowProbe();throw error}};",
            "stream continuations: author algorithms defined independently of implementation");
        static const char *const prepare[]={
            "globalThis.stream=new D.ReadableStream(B.source)",
            "globalThis.stream=new D.ReadableStream(B.bytes)",
            "globalThis.stream=new D.ReadableStream(B.source);globalThis.sink=new D.WritableStream(B.sink)",
            "globalThis.stream=new D.ReadableStream(B.pending);globalThis.sink=new D.WritableStream(B.sink);globalThis.abort=new E.AbortController();globalThis.result=stream.pipeTo(sink,{signal:abort.signal});result.catch(()=>{})",
            "globalThis.encoder=new D.TextEncoderStream();globalThis.decoder=new D.TextDecoderStream()",
            "globalThis.stream=new D.ReadableStream(B.bytes);globalThis.response=new D.Response(stream)",
            "globalThis.stream=new D.ReadableStream(B.source);globalThis.iterator=stream.values()",
            "globalThis.sink=new D.WritableStream(B.throwing)"};
        realm_script(ce_realms[0],prepare[family],"stream continuations: live caller A creates retained data in D");
        os64_js_outcome_t out;
        if(mode!=8)check(os64_js_realm_retire(ce_realms[3],OS64_JS_ABI_ID,&out)==OS64_JS_OK,
              "stream continuations: implementation data owner retires before consumption");
        JS_RunGC(JS_GetRuntime(ce_contexts[0]));
        static const char *const run[]={
            "globalThis.values=[];let branches=A.stream.tee();for(let s of branches)s.getReader().read().then(v=>values.push(v.value))",
            "globalThis.values=[];let branches=A.stream.tee();branches[0].getReader({mode:'byob'}).read(new Uint8Array(2)).then(v=>values.push(v.value[0]));branches[1].getReader().read().then(v=>values.push(v.value[1]))",
            "globalThis.done=0;globalThis.failure=undefined;A.stream.pipeTo(A.sink).then(()=>done++,e=>failure=e)",
            "globalThis.done=0;globalThis.failure=undefined;A.result.then(()=>done++,e=>{failure=e;done++});A.abort.abort(B.error)",
            "globalThis.text='';globalThis.failure=undefined;A.encoder.readable.pipeTo(A.decoder.writable).catch(e=>failure=e);let writer=A.encoder.writable.getWriter();writer.write('hello');writer.close();A.decoder.readable.getReader().read().then(v=>text=v.value)",
            "globalThis.text='';A.response.text().then(v=>text=v)",
            "globalThis.value=0;A.iterator.next().then(v=>value=v.value)",
            "globalThis.done=0;globalThis.failure=undefined;A.sink.abort().then(()=>done++,e=>{failure=e;done++})"};
        check(os64_js_realm_run(ce_realms[4],run[family],strlen(run[family]),"stream-consume",&out)==OS64_JS_OK,
              "stream continuations: E consumes retained data and schedules algorithms");
        static const char *const verify[]={
            "assert(values.length===2&&values[0]===7&&values[1]===7)",
            "assert(values.length===2&&values.includes(7)&&values.includes(8))",
            "assert(done===1&&failure===undefined&&B.writes.length===1&&B.writes[0]===7)",
            "assert(done===1&&failure===B.error)",
            "assert(text==='hello'&&failure===undefined)",
            "assert(text==='\\u0007\\b')",
            "assert(value===7)",
            "assert(done===1&&failure===B.error)"};
        char label[128];snprintf(label,sizeof(label),"stream continuations: departure completion policy=%d family=%u",policy,mode);
        realm_script(ce_realms[4],verify[family],label);
        if(family==7)check(flow_calls==1,"stream continuations: live B throws once in both lifetime controls");
        if(mode!=4)check(flow_calls>0,"stream continuations: author callbacks reached checked scope");
        check(!JS_GetEntryContext(JS_GetRuntime(ce_contexts[0]))&&!JS_GetIncumbentContext(JS_GetRuntime(ce_contexts[0])),
              "stream continuations: private and callback scopes unwind");ce_close(&f);
    }
}

static unsigned flow_conversion_mode, flow_gets, flow_then_calls, flow_species_calls;
static JSValue flow_conversion_probe(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv,int phase,JSValue *data)
{
    (void)self;(void)argc;(void)argv;(void)data;os64_js_outcome_t out;
    os64_js_realm_t *entry=os64_js_settings_realm(ce_runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_ENTRY,&out);
    os64_js_realm_t *incumbent=os64_js_settings_realm(ce_runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_INCUMBENT,&out);
    check(os64_js_settings_realm(ce_runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_CURRENT,&out)==ce_realms[2],
          "stream conversion: observable getter/callback current remains C");
    if(phase==2) {
        flow_species_calls++;
        check((!entry||entry==ce_realms[3]||entry==ce_realms[4])&&(!incumbent||incumbent==ce_realms[4]),
              "stream conversion: species effects do not inherit finished callback settings");
        return sc_property(ce_contexts[4],"Promise");
    }
    check(entry==ce_realms[4]&&incumbent==ce_realms[4],
          "stream conversion: post-cleanup getter and live then use E, not callback B or captured A");
    check(ctx==ce_contexts[phase==0?3:4],"stream conversion: native incoming context is independent of getter realm C");
    if(phase==1){flow_then_calls++;return JS_UNDEFINED;}
    flow_gets++;
    if(flow_conversion_mode==3)return JS_Throw(ctx,sc_property(ce_contexts[1],"error"));
    if(flow_conversion_mode==1||flow_conversion_mode==4) {
        JSValue ctor=sc_property(ce_contexts[flow_conversion_mode==1?4:3],"Promise");
        JSValue proto=JS_GetPropertyStr(ctx,ctor,"prototype"),then=JS_GetPropertyStr(ctx,proto,"savedThen");
        JS_FreeValue(ctx,proto);JS_FreeValue(ctx,ctor);return then;
    }
    return sc_property(ce_contexts[flow_conversion_mode==2?3:4],"customThen");
}
static void dom_stream_conversion_observation_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++)
    for(unsigned mode=0;mode<7;mode++) {
        CeFixture f;if(!ce_open(&f,policy,7)){ce_close(&f);continue;}
        ce_incumbent=1;flow_calls=flow_gets=flow_then_calls=flow_species_calls=0;flow_conversion_mode=mode;
        JSContext *ctx=ce_contexts[2];JSValue global=JS_GetGlobalObject(ctx);
        check(JS_SetPropertyStr(ctx,global,"flowProbe",JS_NewCFunctionData(ctx,flow_probe,0,0,0,NULL))>=0,
              "stream conversion: underlying callback scope probe installed");
        const char *names[]={"getThen","thenProbe","getSpecies"};
        for(int i=0;i<3;i++)check(JS_SetPropertyStr(ctx,global,names[i],JS_NewCFunctionData(ctx,flow_conversion_probe,0,i,0,NULL))>=0,
                                  "stream conversion: native observation installed");JS_FreeValue(ctx,global);
        realm_script(ce_realms[1],"globalThis.error=new TypeError('conversion');globalThis.sink={abort(){C.flowProbe();throw error}}",
                     "stream conversion: throwing live callback prepared");
        realm_script(ce_realms[3],"globalThis.customThen=function(resolve,reject){C.thenProbe();reject(B.error)};Promise.prototype.savedThen=Promise.prototype.then",
                     "stream conversion: ordinary departed then has its own code realm");
        realm_script(ce_realms[4],"globalThis.customThen=function(resolve,reject){C.thenProbe();reject(B.error)};Promise.prototype.savedThen=Promise.prototype.then",
                     "stream conversion: independent live then has its own code realm");
        realm_script(ce_realms[0],"globalThis.sink=new D.WritableStream(B.sink)","stream conversion: callback captured before mutation");
        if(mode==6)realm_script(ce_realms[4],"Object.defineProperty(D.Promise,Symbol.species,{get:C.getSpecies})",
                               "stream conversion: mutable species remains observable");
        else realm_script(ce_realms[4],"Object.defineProperty(D.Promise.prototype,'then',{get:C.getThen,configurable:true})",
                          "stream conversion: mutable then lookup remains observable");
        os64_js_outcome_t out;
        if(mode<5)check(os64_js_realm_retire(ce_realms[3],OS64_JS_ABI_ID,&out)==OS64_JS_OK,
                        "stream conversion: D retires without retiring live replacement code");
        const char *source="globalThis.done=0;globalThis.failure=undefined;Promise.prototype.savedThen.call(A.sink.abort(),()=>done++,e=>{failure=e;done++})";
        check(os64_js_realm_run(ce_realms[4],source,strlen(source),"stream-observe",&out)==OS64_JS_OK,
              "stream conversion: result observed through live intrinsic without rereading D.then");
        bool discarded=mode==2;
        check(out.jobs_discarded==(unsigned)discarded&&!out.jobs_pending,
              "stream conversion: author then retains retirement; explicit native adoption completes");
        realm_script(ce_realms[4],discarded?"assert(done===0&&failure===undefined&&errors.length===0)":"assert(done===1&&failure===B.error)",
                     "stream conversion: throws and replacement then preserve error identity; dead jobs remain pending");
        check(flow_calls==1&&flow_gets==(unsigned)(mode!=6)&&flow_then_calls==(unsigned)(mode==0||mode==5),
              "stream conversion: callback and observable then getter run once; departed then does not run");
        if(mode==6)check(flow_species_calls>0,"stream conversion: species getter effects preserved");
        ce_close(&f);
    }
}

/* A retained operation must complete both successful callback conversion and
 * thrown-callback conversion after its implementation document departs. */
static void dom_stream_retained_error_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++)
    for(unsigned family=0;family<4;family++)for(unsigned operation=0;operation<2;operation++) {
        CeFixture f;if(!sc_open(&f,policy,family,true)){ce_close(&f);continue;}
        sc_construct(family);os64_js_outcome_t out;
        check(os64_js_realm_retire(ce_realms[3],OS64_JS_ABI_ID,&out)==OS64_JS_OK,
              "retained stream errors: implementation departs before callback throws");
        sc_throw=true;JS_RunGC(JS_GetRuntime(ce_contexts[0]));
        char source[768];snprintf(source,sizeof(source),
            "globalThis.done=0;globalThis.failure=undefined;%s.then(()=>done++,e=>{done++;failure=e})",
            sc_operation(family,operation!=0));
        realm_script(ce_realms[4],source,"retained stream errors: live E invokes live B callback on D data");
        realm_script(ce_realms[4],"assert(done===1&&failure===C.error)",
                     "retained stream errors: throw rejects exactly once with original identity");
        check(sc_calls[operation?2:1]==1,"retained stream errors: author callback runs once");
        check(!JS_IsJobPending(JS_GetRuntime(ce_contexts[0])),"retained stream errors: conversion leaves no queued work");
        ce_close(&f);
    }
}
