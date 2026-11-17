#ifndef OS64_JS_NEUTRAL_NATIVE_JOBS_H
#define OS64_JS_NEUTRAL_NATIVE_JOBS_H

static JSValue native_settings_job(JSContext *ctx,int argc,JSValueConst *argv)
{
    (void)argc;JSValue value=JS_NewInt32(ctx,73);
    return JS_Call(ctx,argv[0],JS_UNDEFINED,1,&value);
}

static void neutral_native_job_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++)
    for(unsigned mode=0;mode<12;mode++) {
        NeutralPromise f;if(!neutral_open(&f,policy,true,mode!=0&&mode!=7&&mode!=10)){neutral_close(&f);continue;}
        os64_js_outcome_t out;JSRuntime *rt=JS_GetRuntime(f.ctx[0]);
        f.nested_throw=mode==2;f.cancel=mode==4;f.retired=mode==3;
        if(mode==7)f.enabled=false;
        if(mode==9) {
            check(realm_execute(f.realm[2],"globalThis.error=new TypeError('native job');cap=x=>{probe(x);throw error}",&out)==OS64_JS_OK,
                  "native jobs: independent throwing code prepared");
            const os64_js_script_host_t host={.opaque=&f,.exception=neutral_exception};
            check(os64_js_realm_set_script_host(f.realm[3],OS64_JS_ABI_ID,&host,&out)==OS64_JS_OK,
                  "native jobs: existing diagnostic fallback installed");
        }
        if(mode==10) {
            JSValue global=JS_GetGlobalObject(f.ctx[2]);
            check(JS_SetPropertyStr(f.ctx[2],global,"cap",JS_NewCFunction(f.ctx[2],neutral_raw_capability,"cap",1))>=0,
                  "native jobs: nested raw-drain probe installed");JS_FreeValue(f.ctx[2],global);
        }
        check(os64_js_realm_task_begin(f.realm[0],OS64_JS_ABI_ID,"native-job",&out)==OS64_JS_OK,
              "native jobs: task begins independently of queue context");
        JSValue fn=realm_property(f.ctx[2],"cap");
        JSJobGroup group;JS_InitJobGroup(&group,f.ctx[1]);
        if(mode==11)check(!JS_SetJobGroup(rt,&group),"native jobs: candidate group selected");
        int queued=mode==7?JS_EnqueueJobOwned(f.ctx[3],NULL,NULL,native_settings_job,1,&fn)
                          :JS_EnqueueJobNeutralSettings(f.ctx[3],native_settings_job,1,&fn);
        if(mode==11)check(JS_SetJobGroup(rt,NULL)==&group,"native jobs: enqueue group restored");
        JS_FreeValue(f.ctx[2],fn);check(queued==0,"native jobs: callable retained by queue");
        JSContext *entry=f.ctx[0],*incumbent=f.ctx[0],*report=f.ctx[0];
        check(JS_PeekJobCallbackSettings(rt,mode==11?&group:NULL,&entry,&incumbent,&report)==(mode!=7)&&
              !entry&&!incumbent&&!report,"native jobs: explicit admission with absent entry backup and report");
        if(mode==3||mode==5)check(os64_js_realm_retire(f.realm[mode==3?2:3],OS64_JS_ABI_ID,&out)==OS64_JS_OK,
                                 "native jobs: execution and queue context retirement are distinct");
        if(mode==8){JS_EnableExecutionScopeTracking(rt);check(JS_MarkContextExecutableFailed(f.ctx[2]),"native jobs: author executable sealed");}
        JS_RunGC(rt);
        if(mode==6){neutral_close(&f);continue;}
        if(mode==10) {
            JSValue result=JS_UNDEFINED;
            check(JS_RunWithSettings(f.ctx[0],f.ctx[0],neutral_outer_scope,NULL,&result)&&!JS_IsException(result),
                  "native jobs: nested neutral drain restores enclosing settings");JS_FreeValue(f.ctx[0],result);
        }
        if(mode==11)check(JS_DiscardJobGroup(rt,&group)==1&&!JS_IsJobPending(rt),
                          "native jobs: grouped discard releases queue and group linkage without calling author");
        check(os64_js_task_end(f.runtime,OS64_JS_ABI_ID,&out)==
              (mode==4?OS64_JS_CANCELLED:mode==8||mode==9?OS64_JS_EXCEPTION:OS64_JS_OK),
              "native jobs: normal throw cancellation and failure-seal outcomes preserved");
        check(f.calls==(unsigned)(mode!=8&&mode!=11),"native jobs: admitted code executes once; sealed/discarded code does not");
        check(!JS_GetEntryContext(rt)&&!JS_GetCurrentContext(rt)&&!JS_GetIncumbentContext(rt),
              "native jobs: settings leave no borrowed scope after completion");
        if(mode==9)check(f.reports==1,"native jobs: thrown identity reports through existing queue-context fallback");
        neutral_close(&f);
    }
}
#endif
