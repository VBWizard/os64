#ifndef OS64_JS_NEUTRAL_PROMISE_SETTINGS_CASES_H
#define OS64_JS_NEUTRAL_PROMISE_SETTINGS_CASES_H
/* A creates the promise, B supplies its species, C supplies the capability,
 * and D settles it. An absent handler supplies none of those as job entry. */
typedef struct {
    os64_js_runtime_t *runtime;
    os64_js_realm_t *realm[4];
    JSContext *ctx[4];
    unsigned calls, nested_calls, reports;
    bool enabled, script, nested_throw, cancel, retired;
} NeutralPromise;
static NeutralPromise *neutral_promise;

static JSValue neutral_nested(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;(void)argc;(void)argv;
    NeutralPromise *f=neutral_promise;os64_js_outcome_t out;
    check(os64_js_settings_realm(f->runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_ENTRY,&out)==f->realm[1]&&
          os64_js_settings_realm(f->runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_INCUMBENT,&out)==f->realm[2]&&
          os64_js_settings_realm(f->runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_CURRENT,&out)==f->realm[1],
          "neutral Promise: nested native callback has explicit B entry and captured C incumbent");
    f->nested_calls++;
    return f->nested_throw?JS_ThrowTypeError(ctx,"nested callback"):JS_UNDEFINED;
}

static JSValue neutral_probe(JSContext *ctx, JSValueConst self, int argc,
                             JSValueConst *argv, int magic, JSValue *data)
{
    (void)self;(void)magic;(void)data;
    NeutralPromise *f=neutral_promise;os64_js_outcome_t out;
    f->calls++;
    check(argc==1&&JS_IsNumber(argv[0])&&JS_VALUE_GET_INT(argv[0])==73,
          "neutral Promise: capability receives forwarded fulfillment or rejection");
    check(os64_js_settings_realm(f->runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_ENTRY,&out)==NULL&&
          out.status==OS64_JS_ADMISSION_REFUSED,"neutral Promise: no invented job entry");
    check(os64_js_settings_realm(f->runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_CURRENT,&out)==
          (f->enabled&&!f->retired?f->realm[2]:NULL),"neutral Promise: checked current follows actual capability C");
    if(f->retired)check(out.status==OS64_JS_REALM_RETIRED,"neutral Promise: retired capability cannot provide checked settings");
    bool capture=f->enabled&&f->script&&!f->retired;
    check(os64_js_settings_realm(f->runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_INCUMBENT,&out)==
          (capture?f->realm[2]:NULL),"neutral Promise: script supplies incumbent; native capability has no backup");
    check(JS_GetCurrentContext(JS_GetRuntime(ctx))==(f->enabled?f->ctx[2]:NULL),
          "neutral Promise: raw current remains opt-in and independent of checked lifecycle");
    check(JS_GetNativeCallerContext(JS_GetRuntime(ctx))==(f->enabled&&f->script?f->ctx[2]:NULL),
          "neutral Promise: native caller follows script execution, not the job's dispatch context");
    check(os64_js_settings_realm(f->runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_NATIVE_CALLER,&out)==
          (capture?f->realm[2]:NULL),"neutral Promise: checked native caller retains lifecycle and missing-context refusal");
    check(ctx==f->ctx[f->script?2:3],"neutral Promise: CFunctionData incoming context does not select current");
    check(os64_js_task_end(f->runtime,OS64_JS_ABI_ID,&out)==OS64_JS_BUSY,
          "neutral Promise: capability cannot close borrowed task");
    JSValue fn=realm_property(f->ctx[1],"nested");
    os64_js_callback_t callback=OS64_JS_CALLBACK_INIT;
    os64_js_status_t status=os64_js_callback_capture(f->runtime,OS64_JS_ABI_ID,fn,&callback,&out);
    check(status==(capture?OS64_JS_OK:f->retired&&f->script?OS64_JS_REALM_RETIRED:OS64_JS_ADMISSION_REFUSED),
          "neutral Promise: callback capture requires an eligible actual incumbent");
    if(status==OS64_JS_OK) {
        JSValue result=JS_UNDEFINED;
        status=os64_js_callback_with_settings(f->realm[1],OS64_JS_ABI_ID,&callback,
            promise_settings_nested,NULL,&result,&out);
        check(status==(f->nested_throw?OS64_JS_EXCEPTION:OS64_JS_OK),
              "neutral Promise: nested callback result preserved");
        if(JS_IsException(result))JS_FreeValue(ctx,JS_GetException(ctx));
        JS_FreeValue(ctx,result);
        check(os64_js_callback_clear(f->runtime,OS64_JS_ABI_ID,&callback,&out)==OS64_JS_OK,
              "neutral Promise: nested capture released");
        check(JS_GetEntryContext(JS_GetRuntime(ctx))==NULL&&
              JS_GetCurrentContext(JS_GetRuntime(ctx))==f->ctx[2]&&
              JS_GetIncumbentContext(JS_GetRuntime(ctx))==f->ctx[2],
              "neutral Promise: success and throw unwind to absent entry and actual C settings");
    }
    JS_FreeValue(ctx,fn);
    if(f->cancel)os64_js_cancel(f->runtime);
    return JS_UNDEFINED;
}

static bool neutral_open(NeutralPromise *f,int policy,int enabled,bool script)
{
    *f=(NeutralPromise){.enabled=enabled,.script=script};neutral_promise=f;
    os64_js_config_t config=fixture_config();os64_js_outcome_t out;
    check(os64_js_create_with_teardown(&config,policy,OS64_JS_ABI_ID,&f->runtime,&out)==OS64_JS_OK,
          "neutral Promise: runtime created");if(!f->runtime)return false;
    check(os64_js_enable_neutral_promise_settings(f->runtime,"wrong-abi",&out)==OS64_JS_ABI_MISMATCH,
          "neutral Promise: configuration checks ABI");
    check((enabled==2?os64_js_enable_browser_jobs(f->runtime,OS64_JS_ABI_ID,&out):
           enabled?os64_js_enable_neutral_promise_settings(f->runtime,OS64_JS_ABI_ID,&out):
                   os64_js_enable_import_settings(f->runtime,OS64_JS_ABI_ID,&out))==OS64_JS_OK,
          "neutral Promise: explicit configuration selected before execution");
    f->realm[0]=os64_js_primary_realm(f->runtime,OS64_JS_ABI_ID,&out);
    for(unsigned i=0;i<4;i++) {
        if(i)check(os64_js_realm_create(f->runtime,OS64_JS_ABI_ID,&f->realm[i],&out)==OS64_JS_OK,"neutral Promise: realm created");
        f->ctx[i]=os64_js_realm_context(f->realm[i],OS64_JS_ABI_ID,&out);
        if(!f->ctx[i])return false;
    }
    for(unsigned i=0;i<4;i++) {
        JSValue global=JS_GetGlobalObject(f->ctx[i]);
        for(unsigned j=0;j<4;j++){char name[]={(char)('A'+j),0};
            check(JS_SetPropertyStr(f->ctx[i],global,name,JS_GetGlobalObject(f->ctx[j]))>=0,"neutral Promise: trusted fixture globals shared");}
        if(i==2)check(JS_SetPropertyStr(f->ctx[i],global,"probe",JS_NewCFunctionData(f->ctx[i],neutral_probe,1,0,0,NULL))>=0,
                      "neutral Promise: capability probe installed");
        if(i==1)check(JS_SetPropertyStr(f->ctx[i],global,"nested",JS_NewCFunction(f->ctx[i],neutral_nested,"nested",0))>=0,
                      "neutral Promise: nested native callback installed");
        JS_FreeValue(f->ctx[i],global);
    }
    check(realm_execute(f->realm[2],script?"globalThis.cap=x=>probe(x)":"globalThis.cap=probe",&out)==OS64_JS_OK,
          "neutral Promise: executable capability initialized");
    check(realm_execute(f->realm[1],"globalThis.Species=function(executor){executor(C.cap,C.cap)}",&out)==OS64_JS_OK,
          "neutral Promise: species belongs to B");
    check(realm_execute(f->realm[0],"globalThis.pending=new Promise((r,j)=>{D.resolve=r;D.reject=j});pending.constructor={[Symbol.species]:B.Species}",&out)==OS64_JS_OK,
          "neutral Promise: source promise belongs to A with foreign species");
    return true;
}

static void neutral_close(NeutralPromise *f)
{
    if(!f->runtime)return;
    JSRuntime *engine=JS_GetRuntime(f->ctx[0]);
    check(!JS_GetEntryContext(engine)&&!JS_GetCurrentContext(engine)&&!JS_GetIncumbentContext(engine),
          "neutral Promise: no settings survive the checkpoint");
    os64_js_teardown_report_t report={0};os64_js_destroy_report(f->runtime,&report);
    check(!report.leaked,"neutral Promise: pending and queued capability roots drain");
    neutral_promise=NULL;
}

static JSValue neutral_raw_job(JSContext *ctx,int argc,JSValueConst *argv)
{
    (void)argc;(void)argv;os64_js_outcome_t out;
    check(!JS_GetCurrentContext(JS_GetRuntime(ctx))&&
          !os64_js_settings_realm(neutral_promise->runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_CURRENT,&out)&&
          out.status==OS64_JS_ADMISSION_REFUSED,"neutral Promise: generic raw job remains unprepared");
    return JS_UNDEFINED;
}

static void neutral_exception(void *opaque,JSContext *ctx,JSValue error)
{
    NeutralPromise *f=opaque;f->reports++;
    JSValue expected=realm_property(f->ctx[2],"error");
    check(ctx==f->ctx[3]&&JS_IsObject(error)&&JS_VALUE_GET_PTR(error)==JS_VALUE_GET_PTR(expected),
          "neutral Promise: diagnostic fallback is queue D while exception identity remains C");
    JS_FreeValue(ctx,expected);
}

static JSValue neutral_raw_capability(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv)
{
    (void)self;(void)argc;(void)argv;neutral_promise->calls++;
    JSRuntime *engine=JS_GetRuntime(ctx);
    check(!JS_GetEntryContext(engine)&&!JS_GetIncumbentContext(engine)&&
          JS_GetCurrentContext(engine)==neutral_promise->ctx[2],
          "neutral Promise: raw nested drain does not borrow outer host entry or backup");
    return JS_UNDEFINED;
}
static JSValue neutral_outer_scope(JSContext *ctx,void *opaque)
{
    (void)opaque;JSRuntime *engine=JS_GetRuntime(ctx);JSContext *job=NULL;
    check(JS_ExecutePendingJob(engine,&job)==1,"neutral Promise: raw nested drain executes one reaction");
    check(JS_GetEntryContext(engine)==ctx&&JS_GetCurrentContext(engine)==ctx&&JS_GetIncumbentContext(engine)==ctx,
          "neutral Promise: raw nested drain restores outer host settings");
    return JS_UNDEFINED;
}

static void neutral_promise_settings_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++)
    for(unsigned mode=0;mode<18;mode++) {
        NeutralPromise f;if(!neutral_open(&f,policy,mode!=4,mode!=3&&mode!=17)){neutral_close(&f);continue;}
        os64_js_outcome_t out;
        f.nested_throw=mode==5;f.cancel=mode==10;
        if(mode==12) {
            check(realm_execute(f.realm[2],"globalThis.error=new TypeError('capability');cap=x=>{probe(x);throw error}",&out)==OS64_JS_OK,
                  "neutral Promise: throwing capability compiled before capture");
            const os64_js_script_host_t host={.opaque=&f,.exception=neutral_exception};
            check(os64_js_realm_set_script_host(f.realm[3],OS64_JS_ABI_ID,&host,&out)==OS64_JS_OK,
                  "neutral Promise: queue-context diagnostic host installed");
        }
        if(mode==15) {
            JSValue global=JS_GetGlobalObject(f.ctx[2]);
            check(JS_SetPropertyStr(f.ctx[2],global,"cap",JS_NewCFunction(f.ctx[2],neutral_raw_capability,"cap",1))>=0,
                  "neutral Promise: raw nested drain capability installed");
            JS_FreeValue(f.ctx[2],global);
        }
        if(mode==13||mode==14)check(realm_execute(f.realm[1],mode==13?
            "C.cap=C.cap.bind(null)":"C.cap=new Proxy(C.cap,{})",&out)==OS64_JS_OK,
            "neutral Promise: bound or proxy capability preserves executable C");
        check(os64_js_realm_task_begin(f.realm[3],OS64_JS_ABI_ID,"neutral-promise",&out)==OS64_JS_OK,
              "neutral Promise: independent task opens");
        if(mode==2)check(promise_settings_eval(f.realm[3],"resolve(73)",&out)==OS64_JS_OK,"neutral Promise: settlement before registration");
        check(promise_settings_eval(f.realm[3],"A.pending.then(null,null)",&out)==OS64_JS_OK,
              "neutral Promise: absent handlers register without capturing D");
        if(mode!=2&&mode!=7)check(promise_settings_eval(f.realm[3],mode==1?"reject(73)":"resolve(73)",&out)==OS64_JS_OK,
                                   "neutral Promise: D settles source promise");
        if(mode==8||mode==9) {
            f.retired=mode==9;
            check(os64_js_realm_retire(f.realm[mode==8?3:2],OS64_JS_ABI_ID,&out)==OS64_JS_OK,
                  "neutral Promise: settlement or capability realm retires");
        }
        if(mode>=16) {
            JS_EnableExecutionScopeTracking(JS_GetRuntime(f.ctx[0]));
            check(JS_MarkContextExecutableFailed(f.ctx[2]),
                  "neutral Promise: capability context sealed after unwind");
        }
        JS_RunGC(JS_GetRuntime(f.ctx[0]));
        if(mode==6||mode==7){neutral_close(&f);continue;}
        JSContext *entry=f.ctx[0],*incumbent=f.ctx[0],*report=f.ctx[0];
        check(JS_PeekJobCallbackSettings(JS_GetRuntime(f.ctx[0]),NULL,&entry,&incumbent,&report)==f.enabled&&
              !entry&&!incumbent&&!report,"neutral Promise: queue metadata keeps absent handler settings absent");
        if(mode==15) {
            JSValue result=JS_UNDEFINED;
            check(JS_RunWithSettings(f.ctx[0],f.ctx[0],neutral_outer_scope,NULL,&result)&&!JS_IsException(result),
                  "neutral Promise: explicit outer host scope completes");
            JS_FreeValue(f.ctx[0],result);
        }
        if(mode==11)check(JS_EnqueueJob(f.ctx[0],neutral_raw_job,0,NULL)==0,"neutral Promise: raw sibling queued");
        check(os64_js_task_end(f.runtime,OS64_JS_ABI_ID,&out)==(mode==10?OS64_JS_CANCELLED:mode==12||mode>=16?OS64_JS_EXCEPTION:OS64_JS_OK),
              "neutral Promise: checkpoint outcome preserves cancellation");
        check(f.calls==(unsigned)(mode<16)&&f.nested_calls==(unsigned)(f.enabled&&f.script&&!f.retired&&mode!=15&&mode<16),
              "neutral Promise: capability and eligible nested callback execute once");
        check(f.reports==(unsigned)(mode==12),"neutral Promise: abrupt capability reported once");
        neutral_close(&f);
    }
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++) {
        os64_js_config_t config=fixture_config();config.limits.jobs_per_turn=1;
        os64_js_runtime_t *runtime=NULL;os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config,policy,OS64_JS_ABI_ID,&runtime,&out)==OS64_JS_OK,
              "neutral Promise: limited runtime created");
        if(!runtime)continue;
        check(os64_js_enable_neutral_promise_settings(runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,
              "neutral Promise: limited runtime opts in");
        const char source[]="Promise.resolve(73).then(null).then(null)";
        check(os64_js_run(runtime,source,sizeof(source)-1,"neutral-budget",&out)==OS64_JS_LIMIT&&
              out.limit==OS64_JS_LIMIT_JOBS&&out.jobs_executed==1,
              "neutral Promise: empty handlers share the turn's job cap");
        os64_js_teardown_report_t report={0};os64_js_destroy_report(runtime,&report);
        check(!report.leaked,"neutral Promise: budget refusal drains remaining capability roots");
    }

}

static JSValue browser_raw_throw(JSContext *ctx,int argc,JSValueConst *argv)
{
    (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx,"raw sibling");
}

/* Policy must apply at each host checkpoint entry, without changing the
 * exception value or assigning authority to the realm used for diagnostics. */
static void browser_job_reporting_cases(void)
{
    for (int policy=OS64_JS_TEARDOWN_FATAL; policy<=OS64_JS_TEARDOWN_RECLAIM; policy++)
    for (int mode=0; mode<3; mode++) {
        NeutralPromise f;
        if (!neutral_open(&f,policy,2,true)) { neutral_close(&f); continue; }
        os64_js_outcome_t out;
        RealmReports hosts[4] = {0};
        for (int i=0;i<4;i++) { hosts[i].context=f.ctx[i]; realm_host(f.realm[i],&hosts[i]); }
        check(realm_execute(f.realm[2],"globalThis.error=new Error('neutral-capability');"
              "cap=x=>{probe(x);throw error}",&out)==OS64_JS_OK,"browser jobs: throwing capability prepared");
        check(realm_execute(f.realm[0],"pending.then()",&out)==OS64_JS_OK,"browser jobs: absent handler registered");
        if(mode)check(os64_js_realm_task_begin(f.realm[3],OS64_JS_ABI_ID,"settlement",&out)==OS64_JS_OK,
                      "browser jobs: explicit task opens");
        check((mode ? promise_settings_eval(f.realm[3],"resolve(73)",&out) :
              os64_js_eval(f.runtime,"D.resolve(73)",13,"settlement",&out))==OS64_JS_OK,
              "browser jobs: foreign resolver settles without an implicit checkpoint");
        os64_js_status_t result=mode==0?os64_js_drain_jobs(f.runtime,64,&out):
            mode==1?os64_js_checkpoint(f.runtime,OS64_JS_ABI_ID,&out):
                    os64_js_task_end(f.runtime,OS64_JS_ABI_ID,&out);
        check(result==OS64_JS_EXCEPTION && fixture_contains(out.message,"neutral-capability"),
              "browser jobs: abrupt neutral job keeps host diagnostic");
        check(f.calls==1 && hosts[0].exceptions+hosts[1].exceptions+hosts[2].exceptions+hosts[3].exceptions==0,
              "browser jobs: no guessed window receives a neutral error");
        if(mode==1)check(os64_js_task_end(f.runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,
                         "browser jobs: checkpoint does not close task");
        check(os64_js_enable_browser_jobs(f.runtime,OS64_JS_ABI_ID,&out)==OS64_JS_BUSY,
              "browser jobs: policy cannot change after execution");
        /* Generic native jobs are not neutral Promise reactions: retain their
         * original reporting destination even when preparation is absent. */
        check(os64_js_realm_task_begin(f.realm[3],OS64_JS_ABI_ID,"raw sibling",&out)==OS64_JS_OK,
              "browser jobs: raw sibling task begins");
        check(JS_EnqueueJob(f.ctx[3], browser_raw_throw, 0, NULL)==0,"browser jobs: raw sibling queued");
        check(os64_js_task_end(f.runtime,OS64_JS_ABI_ID,&out)==OS64_JS_EXCEPTION && hosts[3].exceptions==1,
              "browser jobs: raw job report policy preserved");
        neutral_close(&f);
    }
}

#endif
