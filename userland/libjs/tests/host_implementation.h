#ifndef OS64_JS_HOST_IMPLEMENTATION_CASES_H
#define OS64_JS_HOST_IMPLEMENTATION_CASES_H

static int host_code_number(JSContext *ctx)
{
    for(int i=0;i<3;i++)if(ctx==active_script->ctx[i])return i+1;
    return 0;
}
static JSValue host_code_probe(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv)
{
    (void)self;(void)argc;(void)argv;JSRuntime *rt=JS_GetRuntime(ctx);
    JSNativeCallContexts call;check(JS_GetNativeCallContexts(rt,&call),"host code: physical call facts remain available");
    JSValue record=JS_GetActiveScriptOrModule(rt);
    int value=1000*host_code_number(JS_GetEntryContext(rt))+100*host_code_number(JS_GetIncumbentContext(rt))+
              10*host_code_number(call.incoming)+host_code_number(JS_GetScriptOrModuleContext(record));
    JS_FreeValue(ctx,record);return JS_NewInt32(ctx,value);
}

static void host_implementation_cases(void)
{
    static const char source[]=
        "globalThis.host=()=>snapshot();globalThis.make=()=>()=>snapshot();"
        "globalThis.reenter=fn=>[snapshot(),fn(),snapshot()];"
        "globalThis.gen=function*(){yield snapshot();return snapshot()};"
        "globalThis.later=async()=>{await 0;return snapshot()};"
        "globalThis.dynamic=()=>Function('return snapshot()');"
        "globalThis.fail=()=>{throw new Error('host failure')};";
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++)
    for(unsigned cached=0;cached<3;cached++) {
        ActiveScriptFixture f;if(!active_open(&f,policy)){active_close(&f);continue;}
        JSContext *ctx=f.ctx[1];JSValue global=JS_GetGlobalObject(ctx);
        check(JS_SetPropertyStr(ctx,global,"snapshot",JS_NewCFunction(ctx,host_code_probe,"snapshot",0))>=0,
              "host code: independent settings probe installed");JS_FreeValue(ctx,global);
        JSValue code=JS_Eval(ctx,source,sizeof(source)-1,"host-implementation.js",JS_EVAL_TYPE_GLOBAL|JS_EVAL_FLAG_COMPILE_ONLY);
        check(!JS_IsException(code),"host code: trusted source compiled");
        JSValue alias=JS_DupValue(ctx,code);
        check(!JS_PrepareHostImplementation(ctx,code),"host code: shared tree refused before mutation");JS_FreeValue(ctx,alias);
        check(!JS_PrepareHostImplementation(f.ctx[0],code),"host code: foreign realm refused");
        check(!JS_PrepareHostImplementation(ctx,JS_UNDEFINED),"host code: other values refused");
        JSValue error=JS_NewObject(ctx);JS_Throw(ctx,JS_DupValue(ctx,error));
        check(JS_PrepareHostImplementation(ctx,code),"host code: exclusive tree prepared without allocation");
        JSValue pending=JS_GetException(ctx);check(JS_VALUE_GET_PTR(error)==JS_VALUE_GET_PTR(pending),
              "host code: preparation preserves pending exception identity");JS_FreeValue(ctx,pending);JS_FreeValue(ctx,error);
        if(cached) {
            size_t size=0;uint8_t *bytes=JS_WriteObject(ctx,&size,code,JS_WRITE_OBJ_BYTECODE);
            check(bytes!=NULL,"host code: prepared tree serializes");JS_FreeValue(ctx,code);
            code=bytes?JS_ReadObject(ctx,bytes,size,JS_READ_OBJ_BYTECODE):JS_EXCEPTION;js_free(ctx,bytes);
            check(!JS_IsException(code),"host code: cached tree decoded");
            if(cached==1)check(JS_PrepareHostImplementation(ctx,code),"host code: cached implementation explicitly readmitted");
        }
        active_evaluate_code(&f,1,code);
        const char *checks_source=cached==2?
            "if(B.host()!==1222||B.make()()!==1222)throw 'mark leaked through cache';":
            "if(B.host()!==1121||B.make()()!==1121)throw 'implementation stole caller';"
            "if(B.host.bind(null)()!==1121||new Proxy(B.host,{})()!==1121)throw 'wrapper';"
            "let g=B.gen();if(g.next().value!==1121||g.next().value!==1121)throw 'generator';"
            "let r=B.reenter(C.author);if(r[0]!==1121||r[1]!==1333||r[2]!==1121)throw 'reentry';"
            "try{B.fail()}catch(e){if(recordRealm()!==0)throw 'unwind'}"
            "if(B.dynamic()()!==1221)throw 'dynamic code inherited implementation mark';";
        check(active_run(&f,2,"globalThis.author=()=>B.snapshot()","author-C.js")==OS64_JS_OK,
              "host code: author callback remains ordinary code");
        check(active_run(&f,0,checks_source,"caller-A.js")==OS64_JS_OK,
              "host code: caller provenance survives nested helpers and author reentry");
        if(cached!=2) {
            JSValue fn=realm_property(ctx,"host");os64_js_outcome_t out;
            check(os64_js_realm_task_begin(f.realm[0],OS64_JS_ABI_ID,"native-host",&out)==OS64_JS_OK,
                  "host code: native task opened");
            JSValue result=JS_UNDEFINED;
            check(os64_js_realm_with_settings(f.realm[0],f.realm[0],OS64_JS_ABI_ID,settings_call,&fn,&result,&out)==OS64_JS_OK&&
                  JS_VALUE_GET_INT(result)==1120,"host code: no author frame invents no script record");
            JS_FreeValue(ctx,result);JS_FreeValue(ctx,fn);
            check(os64_js_task_end(f.runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,"host code: native task closes");
            check(active_run(&f,0,"globalThis.answer=0;B.later().then(v=>answer=v)","caller-async.js")==OS64_JS_OK,
                  "host code: async implementation resumes under job settings");
            check(active_run(&f,0,"if(answer!==2121)throw 'async implementation lost captured author record'","verify-async.js")==OS64_JS_OK,
                  "host code: async implementation preserves the job-restored author record");
        }
        if(cached==0) {
            char deep[4096];size_t n=0;
            const char *prefix="globalThis.deep=()=>{";
            os64_memcpy(deep+n,prefix,os64_strlen(prefix));n+=os64_strlen(prefix);
            for(unsigned i=0;i<130;i++){os64_memcpy(deep+n,"function f(){",13);n+=13;}
            for(unsigned i=0;i<130;i++)deep[n++]='}';
            const char *suffix="return snapshot()}";
            os64_memcpy(deep+n,suffix,os64_strlen(suffix)+1);n+=os64_strlen(suffix);
            JSValue nested=JS_Eval(ctx,deep,n,"deep-host.js",JS_EVAL_TYPE_GLOBAL|JS_EVAL_FLAG_COMPILE_ONLY);
            check(!JS_IsException(nested),"host code: deeply nested implementation compiles");
            check(!JS_PrepareHostImplementation(ctx,nested),"host code: bounded preparation refuses excessive depth atomically");
            active_evaluate_code(&f,1,nested);
            check(active_run(&f,0,"if(B.deep()!==1222)throw 'partial preparation'","depth-caller.js")==OS64_JS_OK,
                  "host code: refused tree retains ordinary provenance");
        }
        JS_RunGC(JS_GetRuntime(ctx));active_close(&f);
    }
}
#endif
