#ifndef OS64_JS_HOST_PROMISE_CAPABILITIES_H
#define OS64_JS_HOST_PROMISE_CAPABILITIES_H

/* A is the caller, B supplies mutable author code, C the value/error, and D
 * owns the private capability and the default intrinsic being adopted. */
static JSValue host_promise_make(JSContext *ctx,JSValueConst self,int argc,
                                 JSValueConst *argv,int host)
{
    (void)self;
    JSValue functions[2];
    JSValue promise=host?JS_NewHostPromiseCapability(ctx,functions):JS_NewPromiseCapability(ctx,functions);
    if(JS_IsException(promise))return promise;
    check(JS_GetFunctionRealm(ctx,functions[0])==ctx&&JS_GetFunctionRealm(ctx,functions[1])==ctx,
          "host Promise: private forwarding retains actual native function realm");
    JSValue result=JS_Call(ctx,functions[0],JS_UNDEFINED,argc?1:0,argv);
    JS_FreeValue(ctx,functions[0]);JS_FreeValue(ctx,functions[1]);
    if(JS_IsException(result)){JS_FreeValue(ctx,promise);return result;}
    JS_FreeValue(ctx,result);return promise;
}

static JSValue host_promise_species_probe(JSContext *ctx,JSValueConst self,int argc,
                                          JSValueConst *argv,int kind)
{
    (void)self;(void)argc;(void)argv;
    NeutralPromise *f=neutral_promise;os64_js_outcome_t out;
    check(ctx==f->ctx[1],"host Promise: native probe executes in its real function realm");
    if(kind==2){os64_js_cancel(f->runtime);return JS_UNDEFINED;}
    check(!os64_js_settings_realm(f->runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_ENTRY,&out)&&
          out.status==OS64_JS_ADMISSION_REFUSED,"host Promise: species execution has no invented entry");
    check(os64_js_settings_realm(f->runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_CURRENT,&out)==f->realm[1],
          "host Promise: native species probe exposes its actual current realm");
    check(os64_js_settings_realm(f->runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_INCUMBENT,&out)==(kind?f->realm[1]:NULL),
          "host Promise: script species supplies incumbent; native-only species supplies none");
    JSValue fn=realm_property(f->ctx[1],"Species");
    os64_js_callback_t capture=OS64_JS_CALLBACK_INIT;
    check(os64_js_callback_capture(f->runtime,OS64_JS_ABI_ID,fn,&capture,&out)==
          (kind?OS64_JS_OK:OS64_JS_ADMISSION_REFUSED),"host Promise: nested capture requires an actual script incumbent");
    if(kind)check(os64_js_callback_clear(f->runtime,OS64_JS_ABI_ID,&capture,&out)==OS64_JS_OK,
                  "host Promise: nested script capture released");
    return fn;
}

static void host_promise_capability_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++)
    for(unsigned mode=0;mode<26;mode++) {
        NeutralPromise f;if(!neutral_open(&f,policy,true,true)){neutral_close(&f);continue;}
        JSRuntime *rt=JS_GetRuntime(f.ctx[0]);os64_js_outcome_t out;
        JSValue global=JS_GetGlobalObject(f.ctx[3]);
        check(JS_SetPropertyStr(f.ctx[3],global,"make",JS_NewCFunctionMagic(f.ctx[3],host_promise_make,"make",1,JS_CFUNC_generic_magic,mode!=0&&mode!=22))>=0,
              "host Promise: native admission is selected explicitly");JS_FreeValue(f.ctx[3],global);
        check(realm_execute(f.realm[2],"globalThis.error=new TypeError('original');globalThis.value={answer:73}",&out)==OS64_JS_OK,
              "host Promise: foreign identity prepared");
        check(realm_execute(f.realm[1],"globalThis.calls=0;globalThis.gets=0;globalThis.species=0;globalThis.forwarded=0;"
              "globalThis.custom=function(resolve,reject){calls++;reject(C.error)};"
              "globalThis.Species=function(executor){species++;executor(v=>{forwarded++},e=>{forwarded++})}",&out)==OS64_JS_OK,
              "host Promise: author replacement and derived capabilities prepared");
        global=JS_GetGlobalObject(f.ctx[1]);
        const char *names[]={"nativeSpecies","scriptProbe","cancelNative"};
        for(int i=0;i<3;i++)check(JS_SetPropertyStr(f.ctx[1],global,names[i],
            JS_NewCFunctionMagic(f.ctx[1],host_promise_species_probe,names[i],0,JS_CFUNC_generic_magic,i))>=0,
            "host Promise: species role and cancellation probes installed");
        JS_FreeValue(f.ctx[1],global);
        const char *input=mode==2||mode==11?"globalThis.input=new Promise(r=>{globalThis.finish=r})":
              "globalThis.input=Promise.reject(C.error);input.catch(()=>{})";
        check(realm_execute(f.realm[3],input,&out)==OS64_JS_OK,"host Promise: input prepared before departure");
        static const char *const change[]={
            "", "", "D.finish(C.value)",
            "D.input.then=D.Promise.prototype.then.bind(D.input)",
            "D.input.then=new Proxy(D.Promise.prototype.then,{})",
            "D.input.then=B.custom", "D.input.then=B.Promise.prototype.then",
            "Object.defineProperty(D.input,'then',{get(){B.gets++;throw C.error}})",
            };
        if(mode<8&&*change[mode])check(realm_execute(f.realm[0],change[mode],&out)==OS64_JS_OK,
                               "host Promise: ordinary observable then mutation installed");
        const char *source="globalThis.output=D.make(D.input);output.then(()=>{},()=>{})";
        check(os64_js_realm_task_begin(f.realm[0],OS64_JS_ABI_ID,"host-promise",&out)==OS64_JS_OK,
              "host Promise: task opens before native conversion");
        JSJobGroup group;JS_InitJobGroup(&group,f.ctx[1]);
        if(mode==20)check(!JS_SetJobGroup(rt,&group),"host Promise: installation group selected");
        check(promise_settings_eval(f.realm[0],source,&out)==OS64_JS_OK,
              "host Promise: conversion queues adoption without checkpointing");
        if(mode==20)check(JS_SetJobGroup(rt,NULL)==&group,"host Promise: installation group restored");
        if(mode==8||mode==9||mode==10||mode==16||mode==17||mode==18||mode==19||mode>=23) {
            /* Mutate after enqueue: an intrinsic fast path must still execute
             * SpeciesConstructor at job time, not cache or bypass it. */
            const char *mutation=mode==23?
                "D.input.constructor={get [Symbol.species](){B.cancelNative();return B.Species}}":
                mode==24?"D.input.constructor={};Object.defineProperty(D.input.constructor,Symbol.species,{get:B.nativeSpecies})":
                mode==25?"B.Species=function(executor){B.scriptProbe();B.species++;executor(v=>{B.forwarded++},e=>{B.forwarded++})};D.input.constructor={[Symbol.species]:B.Species}":
                mode==16?
                "D.input.constructor={[Symbol.species]:function(executor){B.species++;Promise.resolve().then(()=>B.calls++);executor(v=>{B.forwarded++},e=>{B.forwarded++})}}":
                mode==17?"D.input.then=B.custom;D.input.constructor={[Symbol.species]:B.Species}":
                mode==18?"D.input.constructor={[Symbol.species]:function(){B.species++;throw C.error}}":
                mode==19?"D.input.constructor={[Symbol.species]:B.Species}":mode==8?
                "Object.defineProperty(D.input,'constructor',{get(){B.gets++;return {[Symbol.species]:B.Species}}})":
                mode==9?"D.input.constructor={[Symbol.species]:function(){B.species++;throw C.error}}":
                "D.input.constructor={[Symbol.species]:B.Species}";
            check(promise_settings_eval(f.realm[mode==25?1:0],mutation,&out)==OS64_JS_OK,
                  "host Promise: constructor/species changed after enqueue");
        }
        if(mode!=21&&mode!=22)check(os64_js_realm_retire(f.realm[3],OS64_JS_ABI_ID,&out)==OS64_JS_OK,
              "host Promise: implementation departure keeps data but ends author job eligibility");
        if(mode==5)check(os64_js_realm_retire(f.realm[1],OS64_JS_ABI_ID,&out)==OS64_JS_OK,
                         "host Promise: author replacement owner independently departs");
        if(mode==6)check(os64_js_realm_retire(f.realm[1],OS64_JS_ABI_ID,&out)==OS64_JS_OK,
                         "host Promise: borrowed native intrinsic retains its real departed realm");
        if(mode==12||mode==20){JSContext *a,*b,*c;check(JS_PeekJobCallbackSettings(rt,NULL,&a,&b,&c)&&!a&&!b&&!c,
                "host Promise: private adoption supplies no invented entry incumbent or report");
            check((mode==20?JS_DiscardJobGroup(rt,&group):JS_DiscardPendingJob(rt))==1,
                  "host Promise: explicit or grouped discard releases private adoption");}
        if(mode==13){neutral_close(&f);continue;}
        if(mode==14)os64_js_cancel(f.runtime);
        if(mode==19){JS_EnableExecutionScopeTracking(rt);check(JS_MarkContextExecutableFailed(f.ctx[1]),
              "host Promise: custom species executable is independently sealed");}
        if(mode==15){JS_EnableExecutionScopeTracking(rt);check(JS_MarkContextExecutableFailed(f.ctx[3]),
              "host Promise: ordinary retirement does not disable failed-code sealing");}
        JS_RunGC(rt);
        os64_js_status_t status=os64_js_task_end(f.runtime,OS64_JS_ABI_ID,&out);
        check(status==(mode==14||mode==23?OS64_JS_CANCELLED:mode==15?OS64_JS_EXCEPTION:OS64_JS_OK),
              "host Promise: cancellation and failed native code preserve checked outcomes");
        if(mode==14||mode==15||mode==23){neutral_close(&f);continue;}
        bool discarded=mode==0||mode==3||mode==4||mode==5;
        check(out.jobs_discarded==(unsigned)discarded&&!out.jobs_pending,
              "host Promise: only ordinary then ownership causes retirement discard");
        JSValue output=realm_property(f.ctx[0],"output");
        if(mode==11) {
            check(JS_PromiseState(f.ctx[0],output)==JS_PROMISE_PENDING,"host Promise: pending adoption remains pending");
            check(os64_js_realm_task_begin(f.realm[0],OS64_JS_ABI_ID,"late-settlement",&out)==OS64_JS_OK,
                  "host Promise: later settlement gets a new ordinary task");
            JSValue finish=realm_property(f.ctx[3],"finish"),value=realm_property(f.ctx[2],"value");
            JSValue result=JS_Call(f.ctx[0],finish,JS_UNDEFINED,1,&value);
            check(!JS_IsException(result),"host Promise: live native caller settles retained pending source");
            JS_FreeValue(f.ctx[0],result);JS_FreeValue(f.ctx[0],finish);JS_FreeValue(f.ctx[0],value);
            check(os64_js_task_end(f.runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,
                  "host Promise: retained private forwarding reaction drains");
        }
        bool pending=discarded||mode==12||mode==20;
        if(mode==1||mode==21||mode==22)check(out.jobs_executed==3,
              "host Promise: live and departed private adoption preserve the ordinary three-job timing");
        bool fulfilled=mode==2||mode==11;
        check(JS_PromiseState(f.ctx[0],output)==(pending?JS_PROMISE_PENDING:fulfilled?JS_PROMISE_FULFILLED:JS_PROMISE_REJECTED),
              "host Promise: explicit native conversion completes while ordinary discarded work stays pending");
        if(!pending) {
            JSValue value=JS_PromiseResult(f.ctx[0],output),expected=realm_property(f.ctx[2],fulfilled?"value":"error");
            if(mode!=19)check(JS_VALUE_GET_PTR(value)==JS_VALUE_GET_PTR(expected),"host Promise: result identity survives conversion and GC");
            else check(JS_IsError(f.ctx[0],value),"host Promise: failed species refuses with an error instead of running");
            JS_FreeValue(f.ctx[0],value);JS_FreeValue(f.ctx[0],expected);
        }
        JS_FreeValue(f.ctx[0],output);
        const char *verify=mode==24||mode==25?"if(B.species!==1||B.forwarded!==1)throw 1":mode==16?"if(B.species!==1||B.forwarded!==1||B.calls!==1)throw 1":
            mode==17?"if(B.species!==1||B.forwarded!==1)throw 1":mode==18?"if(B.species!==1)throw 1":
            mode==19?"if(B.species!==0||B.forwarded!==0)throw 1":mode==8?"if(B.gets!==1||B.species!==1||B.forwarded!==1)throw 1":
            mode==9?"if(B.species!==1)throw 1":mode==10?"if(B.species!==1||B.forwarded!==1)throw 1":
            mode==7?"if(B.gets!==1)throw 1":"if(B.calls!==0)throw 1";
        check(realm_execute(f.realm[0],verify,&out)==OS64_JS_OK,"host Promise: getters species and custom capabilities remain observable");
        neutral_close(&f);
    }
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++) {
        os64_js_config_t config=fixture_config();config.limits.jobs_per_turn=1;
        os64_js_runtime_t *runtime=NULL;os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config,policy,OS64_JS_ABI_ID,&runtime,&out)==OS64_JS_OK,
              "host Promise: limited runtime created");
        if(!runtime)continue;
        check(os64_js_enable_neutral_promise_settings(runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,
              "host Promise: limited runtime opts in");
        JSContext *ctx=os64_js_context(runtime,OS64_JS_ABI_ID,&out);
        JSValue global=JS_GetGlobalObject(ctx);
        check(JS_SetPropertyStr(ctx,global,"make",JS_NewCFunctionMagic(ctx,host_promise_make,"make",1,JS_CFUNC_generic_magic,1))>=0,
              "host Promise: limited private factory installed");JS_FreeValue(ctx,global);
        const char source[]="make(Promise.resolve(73)).then(()=>{})";
        check(os64_js_run(runtime,source,sizeof(source)-1,"host-promise-budget",&out)==OS64_JS_LIMIT&&
              out.limit==OS64_JS_LIMIT_JOBS&&out.jobs_executed==1,
              "host Promise: private forwarding shares original job cap");
        os64_js_teardown_report_t report={0};os64_js_destroy_report(runtime,&report);
        check(!report.leaked,"host Promise: job cap refusal releases pending forwarding roots");
    }
}
/* Resolvers can escape through an author thenable. Direct native forwarding
 * remains private; a wrapper must retain ordinary job admission because a
 * proxy trap can introduce author code before the native resolver is called. */
static void host_promise_forwarding_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++)
    for(unsigned mode=0;mode<3;mode++) {
        NeutralPromise f;if(!neutral_open(&f,policy,true,true)){neutral_close(&f);continue;}
        JSValue functions[2],promise=JS_NewHostPromiseCapability(f.ctx[3],functions);
        check(!JS_IsException(promise),"host Promise: escaped private capability created");
        if(JS_IsException(promise)){neutral_close(&f);continue;}
        JSValue global=JS_GetGlobalObject(f.ctx[0]);
        check(JS_SetPropertyStr(f.ctx[0],global,"output",promise)>=0&&
              JS_SetPropertyStr(f.ctx[0],global,"forward",JS_DupValue(f.ctx[0],functions[0]))>=0,
              "host Promise: resolver exposed as an author thenable could retain it");
        JS_FreeValue(f.ctx[3],functions[0]);JS_FreeValue(f.ctx[3],functions[1]);JS_FreeValue(f.ctx[0],global);
        os64_js_outcome_t out;
        const char *source=mode==0?"globalThis.calls=0;globalThis.handler=forward":
            mode==1?"globalThis.calls=0;globalThis.handler=forward.bind(null)":
            "globalThis.calls=0;globalThis.handler=new Proxy(forward,{apply(t,s,a){calls++;return Reflect.apply(t,s,a)}})";
        check(realm_execute(f.realm[0],source,&out)==OS64_JS_OK,"host Promise: author wrapper prepared");
        check(os64_js_realm_retire(f.realm[3],OS64_JS_ABI_ID,&out)==OS64_JS_OK,"host Promise: resolver realm retires");
        check(realm_execute(f.realm[0],"Promise.resolve(73).then(handler)",&out)==OS64_JS_OK&&
              out.jobs_discarded==(unsigned)(mode!=0),
              "host Promise: bound and proxy wrappers cannot inherit private resolver admission");
        JSValue output=realm_property(f.ctx[0],"output");
        check(JS_PromiseState(f.ctx[0],output)==(mode?JS_PROMISE_PENDING:JS_PROMISE_FULFILLED),
              "host Promise: direct forwarding completes; author wrappers keep ordinary retirement");
        JS_FreeValue(f.ctx[0],output);
        check(realm_execute(f.realm[0],"if(calls!==0)throw 1",&out)==OS64_JS_OK,
              "host Promise: author proxy trap did not run through a departed native target");
        neutral_close(&f);
    }
}
#endif
