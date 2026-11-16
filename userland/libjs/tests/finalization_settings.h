#ifndef OS64_JS_FINALIZATION_SETTINGS_CASES_H
#define OS64_JS_FINALIZATION_SETTINGS_CASES_H
#include "os64/js_callbacks.h"

/* A captures, B supplies a proxy or foreign executable, C defines the native
 * callback and D constructs the registry. A source record can differ from B. */
typedef struct {
    os64_js_runtime_t *runtime;
    os64_js_realm_t *realm[4];
    JSContext *ctx[4];
    JSValue record;
    unsigned calls, mode;
} FinalizationSettings;
static FinalizationSettings *finalization_settings;

static JSValue finalization_record(JSContext *ctx, JSValueConst self,int argc,JSValueConst *argv)
{
    (void)self;(void)argc;(void)argv;
    FinalizationSettings *f=finalization_settings;
    JS_FreeValue(ctx,f->record);f->record=JS_GetActiveScriptOrModule(JS_GetRuntime(ctx));
    return JS_UNDEFINED;
}
static JSValue finalization_nested(JSContext *ctx,void *opaque)
{
    FinalizationSettings *f=opaque;unsigned nested=f->mode==6?3:1;
    check(JS_GetEntryContext(JS_GetRuntime(ctx))==f->ctx[nested]&&
          JS_GetIncumbentContext(JS_GetRuntime(ctx))==f->ctx[nested],
          "finalization: nested callback uses explicit settings");
    return JS_UNDEFINED;
}
static JSValue finalization_probe(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv)
{
    FinalizationSettings *f=finalization_settings;JSRuntime *rt=JS_GetRuntime(ctx);
    os64_js_outcome_t out;f->calls++;
    check(ctx==f->ctx[2]&&argc==1&&JS_IsNumber(argv[0])&&JS_VALUE_GET_INT(argv[0])==73,
          "finalization: native C receives held value");
    check(f->mode==1?JS_IsObject(self):JS_IsUndefined(self),"finalization: bound or undefined receiver preserved");
    if(f->mode==11) {
        check(!JS_GetEntryContext(rt)&&!JS_GetIncumbentContext(rt),"finalization: Promise-only opt-in preserves legacy cleanup");
        return JS_UNDEFINED;
    }
    check(os64_js_settings_realm(f->runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_ENTRY,&out)==f->realm[2]&&
          os64_js_settings_realm(f->runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_CURRENT,&out)==f->realm[2],
          "finalization: executable C supplies checked entry and current");
    unsigned selected=f->mode==14?1:0;
    check(os64_js_settings_realm(f->runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_INCUMBENT,&out)==f->realm[selected],
          "finalization: source activation or captured backup supplies incumbent");
    JSValue record=JS_GetActiveScriptOrModule(rt);
    check(JS_VALUE_GET_TAG(record)==JS_VALUE_GET_TAG(f->record)&&
          (JS_IsUndefined(record)||JS_VALUE_GET_PTR(record)==JS_VALUE_GET_PTR(f->record)),
          "finalization: exact construction source survives GC and queueing");
    JS_FreeValue(ctx,record);
    JSValue fn=realm_property(ctx,"cleanup");os64_js_callback_t captured=OS64_JS_CALLBACK_INIT;
    check(os64_js_callback_capture(f->runtime,OS64_JS_ABI_ID,fn,&captured,&out)==OS64_JS_OK&&
          captured.incumbent==f->realm[selected],"finalization: native cleanup can capture a WebIDL-style callback");
    check(os64_js_callback_clear(f->runtime,OS64_JS_ABI_ID,&captured,&out)==OS64_JS_OK,"finalization: nested capture releases");
    JS_FreeValue(ctx,fn);JSValue result=JS_UNDEFINED;
    unsigned nested=f->mode==6?3:1;
    check(os64_js_realm_with_settings(f->realm[nested],f->realm[nested],OS64_JS_ABI_ID,
          finalization_nested,f,&result,&out)==OS64_JS_OK,"finalization: nested callback scope enters");
    JS_FreeValue(ctx,result);
    check(JS_GetEntryContext(rt)==f->ctx[2]&&JS_GetIncumbentContext(rt)==f->ctx[selected],
          "finalization: reentry restores cleanup settings");
    check(os64_js_task_end(f->runtime,OS64_JS_ABI_ID,&out)==OS64_JS_BUSY,
          "finalization: cleanup cannot end its borrowed task");
    if(f->mode==8)return JS_ThrowTypeError(ctx,"cleanup failure");
    if(f->mode==9)os64_js_cancel(f->runtime);
    return JS_UNDEFINED;
}
static bool finalization_open(FinalizationSettings *f,int policy,unsigned mode)
{
    *f=(FinalizationSettings){.record=JS_UNDEFINED,.mode=mode};finalization_settings=f;
    os64_js_config_t config=fixture_config();os64_js_outcome_t out;
    check(os64_js_create_with_teardown(&config,policy,OS64_JS_ABI_ID,&f->runtime,&out)==OS64_JS_OK,"finalization: runtime created");
    if(!f->runtime)return false;
    check((mode==11?os64_js_enable_promise_settings(f->runtime,OS64_JS_ABI_ID,&out):
          os64_js_enable_finalization_settings(f->runtime,OS64_JS_ABI_ID,&out))==OS64_JS_OK,"finalization: selected opt-in configured");
    f->realm[0]=os64_js_primary_realm(f->runtime,OS64_JS_ABI_ID,&out);
    for(unsigned i=0;i<4;i++) {
        if(i)check(os64_js_realm_create(f->runtime,OS64_JS_ABI_ID,&f->realm[i],&out)==OS64_JS_OK,"finalization: child created");
        f->ctx[i]=os64_js_realm_context(f->realm[i],OS64_JS_ABI_ID,&out);if(!f->ctx[i])return false;
    }
    JS_SetGCThreshold(JS_GetRuntime(f->ctx[0]),(size_t)-1);
    for(unsigned i=0;i<4;i++) {
        JSValue global=JS_GetGlobalObject(f->ctx[i]);
        for(unsigned j=0;j<4;j++){char name[]={(char)('A'+j),0};
            check(JS_SetPropertyStr(f->ctx[i],global,name,JS_GetGlobalObject(f->ctx[j]))>=0,"finalization: globals exchanged");}
        check(JS_SetPropertyStr(f->ctx[i],global,"captureRecord",JS_NewCFunction(f->ctx[i],finalization_record,"captureRecord",0))>=0,"finalization: source probe installed");
        if(i==2)check(JS_SetPropertyStr(f->ctx[i],global,"cleanup",JS_NewCFunction(f->ctx[i],finalization_probe,"cleanup",1))>=0,"finalization: native cleanup installed");
        JS_FreeValue(f->ctx[i],global);
    }
    return true;
}
static void finalization_close(FinalizationSettings *f)
{
    if(!f->runtime)return;
    check(!JS_GetEntryContext(JS_GetRuntime(f->ctx[0]))&&!JS_GetIncumbentContext(JS_GetRuntime(f->ctx[0])),
          "finalization: cleanup leaves no settings scopes");
    JS_FreeValue(f->ctx[0],f->record);
    os64_js_teardown_report_t report={0};os64_js_destroy_report(f->runtime,&report);
    check(!report.leaked,"finalization: registry context and record edges release");finalization_settings=NULL;
}
static JSValue finalization_construct(JSContext *ctx,void *opaque)
{
    FinalizationSettings *f=opaque;
    JSValue ctor=realm_property(f->ctx[3],"FinalizationRegistry"),cb=realm_property(f->ctx[2],"cleanup");
    JSValue result=JS_CallConstructor(ctx,ctor,1,&cb);JS_FreeValue(ctx,ctor);JS_FreeValue(ctx,cb);
    return result;
}
static JSValue finalization_eval_body(JSContext *ctx,void *opaque)
{
    const char *source=opaque;
    return JS_Eval(ctx,source,os64_strlen(source),"cleanup-source.js",JS_EVAL_TYPE_GLOBAL);
}
static os64_js_status_t finalization_eval(os64_js_realm_t *realm,const char *source,os64_js_outcome_t *out)
{
    JSContext *ctx=os64_js_realm_context(realm,OS64_JS_ABI_ID,&(os64_js_outcome_t){0});
    JSValue value=JS_UNDEFINED;
    os64_js_status_t result=os64_js_realm_with_settings(realm,NULL,OS64_JS_ABI_ID,
        finalization_eval_body,(void *)source,&value,out);
    if(JS_IsException(value)) {
        JS_FreeValue(ctx,JS_GetException(ctx));
        if(result==OS64_JS_OK)result=OS64_JS_EXCEPTION;
    }
    JS_FreeValue(ctx,value);return result;
}
static void finalization_settings_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++)
    for(unsigned mode=0;mode<18;mode++) {
        FinalizationSettings f;if(!finalization_open(&f,policy,mode)){finalization_close(&f);continue;}
        os64_js_outcome_t out;JSRuntime *rt=JS_GetRuntime(f.ctx[0]);
        RealmReports executable={.context=f.ctx[2]},registry_owner={.context=f.ctx[3]};
        realm_host(f.realm[2],&executable);realm_host(f.realm[3],&registry_owner);
        if(mode==10) {
            JSValue result=finalization_construct(f.ctx[0],&f);
            check(JS_IsException(result),"finalization: unprepared raw author capture refuses");
            if(JS_IsException(result))JS_FreeValue(f.ctx[0],JS_GetException(f.ctx[0]));
            JS_FreeValue(f.ctx[0],result);finalization_close(&f);continue;
        }
        check(realm_execute(f.realm[1],mode==1?"globalThis.handler=C.cleanup.bind({bound:true})":
              "globalThis.pair=Proxy.revocable(C.cleanup,{});globalThis.handler=pair.proxy",&out)==OS64_JS_OK,
              "finalization: proxy or bound callback prepared in B");
        check(os64_js_enable_finalization_settings(f.runtime,OS64_JS_ABI_ID,&out)==OS64_JS_BUSY,
              "finalization: configuration after execution refuses");
        check(os64_js_realm_task_begin(f.realm[0],OS64_JS_ABI_ID,"cleanup-fixture",&out)==OS64_JS_OK,"finalization: task opened");
        if(mode==14) {
            JSValue registry=JS_UNDEFINED;
            check(os64_js_realm_with_settings(f.realm[0],f.realm[1],OS64_JS_ABI_ID,finalization_construct,&f,&registry,&out)==OS64_JS_OK&&!JS_IsException(registry),
                  "finalization: native registration captures B without a source record");
            JSValue global=JS_GetGlobalObject(f.ctx[0]);
            check(JS_SetPropertyStr(f.ctx[0],global,"registry",registry)>=0,"finalization: native registry published");JS_FreeValue(f.ctx[0],global);
        } else {
            const char *source=mode==2?
                "captureRecord();globalThis.construct=B.Function('A.registry=new D.FinalizationRegistry(C.cleanup)');construct()":
                "captureRecord();globalThis.registry=new D.FinalizationRegistry(B.handler)";
            check(finalization_eval(f.realm[0],source,&out)==OS64_JS_OK,"finalization: registry captures construction settings");
        }
        check(finalization_eval(f.realm[0],"globalThis.target={};target.self=target;registry.register(target,73)",&out)==OS64_JS_OK,"finalization: cyclic target registered");
        if(mode==12){finalization_close(&f);continue;}
        if(mode==17) {
            check(finalization_eval(f.realm[0],"registry=null",&out)==OS64_JS_OK,
                  "finalization: registry abandoned with live weak target");
            JS_FreeValue(f.ctx[0],f.record);f.record=JS_UNDEFINED;
            JS_RunGC(rt);JS_RunGC(rt);
            check(!JS_IsJobPending(rt),"finalization: abandoned registry does not schedule cleanup");
            finalization_close(&f);continue;
        }
        if(mode==16)check(finalization_eval(f.realm[1],"pair.revoke()",&out)==OS64_JS_OK,"finalization: callback revoked before enqueue");
        check(finalization_eval(f.realm[0],"target=null",&out)==OS64_JS_OK,"finalization: target released");
        /* Weak-target processing precedes cycle collection. */
        JS_RunGC(rt);JS_RunGC(rt);
        JSJobInfo info;JSContext *entry=NULL,*incumbent=NULL;
        check(JS_PeekPendingJob(rt,&info)&&info.activity==f.ctx[3]&&info.owner==f.ctx[mode==16?3:2],
              "finalization: registry activity and executable job owner remain distinct");
        if(mode!=11)check(JS_PeekJobCallbackSettings(rt,NULL,&entry,&incumbent,NULL)&&entry==f.ctx[mode==16?3:2]&&
              incumbent==f.ctx[mode==2||mode==14?1:0],"finalization: queue preserves captured incumbent independently of executable and source");
        else check(!JS_PeekJobCallbackSettings(rt,NULL,&entry,&incumbent,NULL),"finalization: legacy queue has no captured settings");
        if(mode==13){finalization_close(&f);continue;}
        if(mode>=3&&mode<=6) {
            unsigned retired=mode==3?0:mode==4?2:mode==5?3:1;
            check(os64_js_realm_retire(f.realm[retired],OS64_JS_ABI_ID,&out)==OS64_JS_OK,"finalization: one realm role retires");
        }
        if(mode==7) {
            check(finalization_eval(f.realm[0],"registry=null",&out)==OS64_JS_OK,"finalization: registry released after scheduling");JS_RunGC(rt);
        }
        if(mode==15)check(finalization_eval(f.realm[1],"pair.revoke()",&out)==OS64_JS_OK,"finalization: callback revoked after enqueue");
        os64_js_status_t ended=os64_js_task_end(f.runtime,OS64_JS_ABI_ID,&out);
        check(ended==(mode==9?OS64_JS_CANCELLED:mode==8||mode>=15?OS64_JS_EXCEPTION:OS64_JS_OK),"finalization: cleanup exception or cancellation is reported");
        check(f.calls==((mode>=3&&mode<=5)||mode>=15?0u:1u),"finalization: eligible cleanup delivered once");
        check(executable.exceptions==0&&registry_owner.exceptions==(mode==8||mode==15||mode==16?1u:0u),
              "finalization: cleanup errors report to registry D independently of executable C");
        if(mode>=3&&mode<=5)check(out.jobs_discarded==1,"finalization: ineligible cleanup discarded before execution");
        finalization_close(&f);
    }
}
#endif
