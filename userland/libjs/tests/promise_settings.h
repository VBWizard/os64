#ifndef OS64_JS_PROMISE_SETTINGS_CASES_H
#define OS64_JS_PROMISE_SETTINGS_CASES_H
#include "os64/js_callbacks.h"

/* Capture A, proxy association B, executable C and settlement D deliberately
 * differ. Promise job entry uses GetFunctionRealm, unlike WebIDL callbacks. */
typedef struct {
    os64_js_runtime_t *runtime;
    os64_js_realm_t *realm[4];
    JSContext *ctx[4];
    unsigned calls, expected_incumbent;
    bool nested, thenable, bound, return_thenable;
} PromiseSettings;
static PromiseSettings *promise_settings;

static JSValue promise_settings_cancel(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)ctx;(void)self;(void)argc;(void)argv;
    os64_js_cancel(promise_settings->runtime);return JS_UNDEFINED;
}

static JSValue promise_settings_nested(JSContext *ctx, JSValueConst function, void *opaque)
{
    (void)opaque;return JS_Call(ctx,function,JS_UNDEFINED,0,NULL);
}

static JSValue promise_settings_probe(JSContext *ctx, JSValueConst self, int argc,
                                      JSValueConst *argv, int magic, JSValue *data)
{
    (void)magic;(void)data;
    PromiseSettings *f=promise_settings;os64_js_outcome_t out;
    check(os64_js_settings_realm(f->runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_ENTRY,&out)==f->realm[2],
          "promise settings: job entry follows executable C, not proxy B or settlement D");
    check(os64_js_settings_realm(f->runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_INCUMBENT,&out)==f->realm[f->expected_incumbent],
          "promise settings: incumbent comes from callback capture");
    check(os64_js_settings_realm(f->runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_CURRENT,&out)==f->realm[2]&&ctx==f->ctx[2],
          "promise settings: executable current and incoming job context are C");
    check(os64_js_quiescent(f->runtime,OS64_JS_ABI_ID,&out)==OS64_JS_BUSY,
          "promise settings: callback stays inside original job guard");
    check(os64_js_task_end(f->runtime,OS64_JS_ABI_ID,&out)==OS64_JS_BUSY,
          "promise settings: callback cannot close borrowed task");
    f->calls++;
    if(f->bound||f->thenable) {
        JSValue tag=JS_GetPropertyStr(ctx,self,"tag");const char *text=JS_ToCString(ctx,tag);
        check(text&&os64_strcmp(text,f->thenable?"thenable":"bound")==0,"promise settings: callback receiver preserved");
        JS_FreeCString(ctx,text);JS_FreeValue(ctx,tag);
    } else check(JS_IsUndefined(self),"promise settings: reaction receiver is undefined");
    if(f->nested) {
        f->nested=false;
        JSValue global=JS_GetGlobalObject(f->ctx[1]),fn=JS_GetPropertyStr(ctx,global,"nested");JS_FreeValue(ctx,global);
        os64_js_callback_t captured=OS64_JS_CALLBACK_INIT;JSValue result=JS_UNDEFINED;
        check(os64_js_callback_capture(f->runtime,OS64_JS_ABI_ID,fn,&captured,&out)==OS64_JS_OK,
              "promise settings: native job can register checked DOM-style callback");
        check(os64_js_callback_with_settings(f->realm[1],OS64_JS_ABI_ID,&captured,
              promise_settings_nested,NULL,&result,&out)==OS64_JS_OK,"promise settings: nested callback scope admitted");
        JS_FreeValue(ctx,result);JS_FreeValue(ctx,fn);
        check(os64_js_callback_clear(f->runtime,OS64_JS_ABI_ID,&captured,&out)==OS64_JS_OK,
              "promise settings: nested capture released");
        check(os64_js_settings_realm(f->runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_ENTRY,&out)==f->realm[2],
              "promise settings: nested invocation restores job entry");
    }
    if(f->return_thenable) {
        f->return_thenable=false;f->thenable=true;f->expected_incumbent=2;
        return realm_property(ctx,"nextThenable");
    }
    if(f->thenable) {
        check(argc==2&&JS_IsFunction(ctx,argv[0])&&JS_IsFunction(ctx,argv[1]),"promise settings: thenable receives resolve and reject");
        JSValue arg=JS_NewInt32(ctx,73);return JS_Call(ctx,argv[0],JS_UNDEFINED,1,&arg);
    }
    check(argc==1,"promise settings: reaction receives one argument");
    return argc?JS_DupValue(ctx,argv[0]):JS_UNDEFINED;
}

static bool promise_settings_open(PromiseSettings *f,os64_js_teardown_policy_t policy,bool enabled)
{
    *f=(PromiseSettings){0};promise_settings=f;os64_js_config_t config=fixture_config();
    os64_js_outcome_t out;
    check(os64_js_create_with_teardown(&config,policy,OS64_JS_ABI_ID,&f->runtime,&out)==OS64_JS_OK,
          "promise settings: runtime created");if(!f->runtime)return false;
    check((enabled?os64_js_enable_promise_settings(f->runtime,OS64_JS_ABI_ID,&out)
                  :os64_js_enable_host_settings(f->runtime,OS64_JS_ABI_ID,&out))==OS64_JS_OK,
          "promise settings: selected opt-in enabled before execution");
    f->realm[0]=os64_js_primary_realm(f->runtime,OS64_JS_ABI_ID,&out);
    for(unsigned i=0;i<4;i++) {
        if(i)check(os64_js_realm_create(f->runtime,OS64_JS_ABI_ID,&f->realm[i],&out)==OS64_JS_OK,"promise settings: child realm created");
        f->ctx[i]=os64_js_realm_context(f->realm[i],OS64_JS_ABI_ID,&out);
        if(!f->ctx[i])return false;
    }
    for(unsigned i=0;i<4;i++) {
        JSValue global=JS_GetGlobalObject(f->ctx[i]);
        for(unsigned j=0;j<4;j++){char name[]={(char)('A'+j),0};
            check(JS_SetPropertyStr(f->ctx[i],global,name,JS_GetGlobalObject(f->ctx[j]))>=0,"promise settings: fixture globals exchanged");}
        if(i==2)check(JS_SetPropertyStr(f->ctx[i],global,"probe",JS_NewCFunctionData(f->ctx[i],promise_settings_probe,1,0,0,NULL))>=0,"promise settings: native executable installed");
        if(i==2)check(JS_SetPropertyStr(f->ctx[i],global,"cancel",JS_NewCFunction(f->ctx[i],promise_settings_cancel,"cancel",0))>=0,"promise settings: terminal seam installed");
        JS_FreeValue(f->ctx[i],global);
    }
    check(realm_execute(f->realm[1],"globalThis.pair=Proxy.revocable(C.probe,{});globalThis.handler=pair.proxy;Object.setPrototypeOf(handler,A.Function.prototype);globalThis.nested=()=>{globalThis.nestedRan=true};",&out)==OS64_JS_OK,
          "promise settings: proxy and reentry fixture initialized");
    return true;
}

static void promise_settings_close(PromiseSettings *f)
{
    if(!f->runtime)return;
    check(JS_GetEntryContext(JS_GetRuntime(f->ctx[0]))==NULL&&JS_GetIncumbentContext(JS_GetRuntime(f->ctx[0]))==NULL,
          "promise settings: engine scopes restored after jobs");
    os64_js_teardown_report_t report={0};os64_js_destroy_report(f->runtime,&report);
    check(!report.leaked,"promise settings: pending and queued context holds drain without leaks");
    promise_settings=NULL;
}

static JSValue promise_settings_eval_body(JSContext *ctx, void *opaque)
{
    const char *source=opaque;
    return JS_Eval(ctx,source,os64_strlen(source),"promise-settings",JS_EVAL_TYPE_GLOBAL);
}
static os64_js_status_t promise_settings_eval(os64_js_realm_t *realm,const char *source,os64_js_outcome_t *out)
{
    JSValue value=JS_UNDEFINED;
    os64_js_status_t status=os64_js_realm_with_settings(realm,NULL,OS64_JS_ABI_ID,
        promise_settings_eval_body,(void *)source,&value,out);
    JSContext *ctx=os64_js_realm_context(realm,OS64_JS_ABI_ID,&(os64_js_outcome_t){0});
    if(JS_IsException(value))JS_FreeValue(ctx,JS_GetException(ctx));
    JS_FreeValue(ctx,value);
    return status;
}

static void promise_settings_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++)
    for(unsigned mode=0;mode<20;mode++) {
        PromiseSettings f;if(!promise_settings_open(&f,policy,mode!=11)){promise_settings_close(&f);continue;}
        os64_js_outcome_t out;
        if(mode==10||mode==11) {
            const char source[]="Promise.resolve(1).then(B.handler)";
            JSValue value=JS_Eval(f.ctx[0],source,sizeof(source)-1,"unprepared-promise",JS_EVAL_TYPE_GLOBAL);
            check(JS_IsException(value)==(mode==10),"promise settings: unprepared capture refuses only in the new opt-in");
            if(JS_IsException(value))JS_FreeValue(f.ctx[0],JS_GetException(f.ctx[0]));
            JS_FreeValue(f.ctx[0],value);
            if(mode==10)check(!JS_IsJobPending(JS_GetRuntime(f.ctx[0])),"promise settings: refused capture publishes no job");
            promise_settings_close(&f);continue;
        }
        check(os64_js_realm_task_begin(f.realm[0],OS64_JS_ABI_ID,"promise-settings",&out)==OS64_JS_OK,
              "promise settings: shared task begins");
        if(mode==3) {
            f.thenable=true;
            check(promise_settings_eval(f.realm[1],"globalThis.obj={tag:'thenable',get then(){return handler}};",&out)==OS64_JS_OK,"promise settings: foreign then getter");
            check(promise_settings_eval(f.realm[0],"globalThis.result=Promise.resolve(B.obj);",&out)==OS64_JS_OK,"promise settings: thenable captures after getter returns");
        } else if(mode==15) {
            check(promise_settings_eval(f.realm[0],"globalThis.result=Promise.resolve(19).then(null,{});",&out)==OS64_JS_OK,"promise settings: empty handlers preserve forwarding");
        } else {
            check(promise_settings_eval(f.realm[3],"globalThis.pending=new Promise((resolve,reject)=>{globalThis.resolve=resolve;globalThis.reject=reject});",&out)==OS64_JS_OK,"promise settings: pending promise belongs to D");
            if(mode==1)check(promise_settings_eval(f.realm[3],"resolve(73)",&out)==OS64_JS_OK,"promise settings: promise already settled before capture");
            if(mode==12){f.bound=true;check(promise_settings_eval(f.realm[1],"handler=C.probe.bind({tag:'bound'});",&out)==OS64_JS_OK,"promise settings: bound foreign native handler");}
            if(mode==13)f.nested=true;
            if(mode==19) {
                f.return_thenable=true;
                check(promise_settings_eval(f.realm[2],"globalThis.nextThenable={tag:'thenable',then:B.handler}",&out)==OS64_JS_OK,
                      "promise settings: returned thenable fixture");
            }
            if(mode==14){f.expected_incumbent=2;check(promise_settings_eval(f.realm[2],"globalThis.scriptHandler=x=>probe(x);",&out)==OS64_JS_OK,"promise settings: script-bearing handler");}
            if(mode==16)check(promise_settings_eval(f.realm[2],"globalThis.scriptHandler=()=>{C.cancel();while(true){}};",&out)==OS64_JS_OK,"promise settings: interruptible handler");
            const char *source=mode==14||mode==16?"globalThis.result=D.pending.then(C.scriptHandler);"
                :mode==2?"globalThis.result=D.pending.then(undefined,B.handler);":"globalThis.result=D.pending.then(B.handler);";
            check(promise_settings_eval(f.realm[0],source,&out)==OS64_JS_OK,"promise settings: reaction captures A");
            if(mode==8)check(promise_settings_eval(f.realm[1],"pair.revoke()",&out)==OS64_JS_OK,"promise settings: revoke before enqueue");
            if(mode!=1&&mode!=17)check(promise_settings_eval(f.realm[3],mode==2?"reject(73)":"resolve(73)",&out)==OS64_JS_OK,"promise settings: D settles independently of capture");
            if(mode==9)check(promise_settings_eval(f.realm[1],"pair.revoke()",&out)==OS64_JS_OK,"promise settings: revoke after enqueue");
            if(mode>=4&&mode<=7) {
                unsigned which=mode==4?0:mode==5?2:mode==6?3:1;
                check(os64_js_realm_retire(f.realm[which],OS64_JS_ABI_ID,&out)==OS64_JS_OK,"promise settings: one realm role retires");
            }
        }
        if(mode==17) {
            check(!JS_IsJobPending(JS_GetRuntime(f.ctx[0])),"promise settings: pending reactions are not jobs");
            check(os64_js_task_end(f.runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,"promise settings: pending reaction task closes");
            JS_RunGC(JS_GetRuntime(f.ctx[0]));
            promise_settings_close(&f);continue;
        }
        JS_RunGC(JS_GetRuntime(f.ctx[0]));
        if(mode==18) {
            /* Teardown must release an undelivered queue root. */
            promise_settings_close(&f);continue;
        }
        if(mode!=15) {
            JSContext *entry=NULL,*incumbent=NULL;JSJobInfo info;
            check(JS_PeekPendingJob(JS_GetRuntime(f.ctx[0]),&info)&&
                  JS_PeekPromiseJobSettings(JS_GetRuntime(f.ctx[0]),NULL,&entry,&incumbent)&&
                  incumbent==f.ctx[0]&&entry==f.ctx[mode==8?3:2],
                  "promise settings: queued capture and enqueue-time entry are independent");
        }
        os64_js_status_t ended=os64_js_task_end(f.runtime,OS64_JS_ABI_ID,&out);
        check(ended==(mode==16?OS64_JS_CANCELLED:mode==8||mode==9?OS64_JS_UNHANDLED_REJECTION:OS64_JS_OK),"promise settings: checkpoint preserves terminal or successful outcome");
        unsigned expected=mode==4||mode==5||mode==8||mode==9||mode==15||mode==16?0:mode==19?2:1;
        check(f.calls==expected,"promise settings: role eligibility and callback count");
        if(mode!=16&&mode!=4&&mode!=5) {
            JSValue result=realm_property(f.ctx[0],"result");
            check(JS_PromiseState(f.ctx[0],result)==(mode==8||mode==9?JS_PROMISE_REJECTED:JS_PROMISE_FULFILLED),"promise settings: reaction result state");
            JSValue value=JS_PromiseResult(f.ctx[0],result);
            if(mode!=8&&mode!=9)check(JS_IsNumber(value)&&JS_VALUE_GET_INT(value)==(mode==15?19:73),"promise settings: resolution payload preserved");
            else {
                JSValue ctor=realm_property(f.ctx[mode==8?3:2],"TypeError");
                check(JS_IsInstanceOf(f.ctx[0],value,ctor)==1,"promise settings: revoked errors use enqueue-time job realm");JS_FreeValue(f.ctx[0],ctor);
            }
            JS_FreeValue(f.ctx[0],value);JS_FreeValue(f.ctx[0],result);
        }
        promise_settings_close(&f);
    }
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++) {
        os64_js_config_t config=fixture_config();config.limits.jobs_per_turn=1;
        os64_js_runtime_t *runtime=NULL;os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config,policy,OS64_JS_ABI_ID,&runtime,&out)==OS64_JS_OK,
              "promise settings: budget runtime created");
        if(!runtime)continue;
        check(os64_js_enable_promise_settings(runtime,"wrong-abi",&out)==OS64_JS_ABI_MISMATCH,
              "promise settings: configuration checks caller ABI");
        check(os64_js_enable_promise_settings(runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,
              "promise settings: budget opt-in enabled");
        const char source[]="Promise.resolve(1).then(x=>x+1).then(x=>x+1)";
        check(os64_js_run(runtime,source,sizeof(source)-1,"promise-budget",&out)==OS64_JS_LIMIT&&
              out.limit==OS64_JS_LIMIT_JOBS&&out.jobs_executed==1,
              "promise settings: chained jobs share the original turn cap");
        os64_js_teardown_report_t report={0};os64_js_destroy_report(runtime,&report);
        check(!report.leaked,"promise settings: budget failure drains captured jobs");
    }

}
#endif
