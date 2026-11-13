/* Safe-point checks must observe engine work without changing its outcome. */
static os64_js_runtime_t *quiescence_runtime;
static os64_js_outcome_t *quiescence_outer;
static unsigned quiescence_calls;
static JSValue quiescence_probe(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)ctx;(void)self;(void)argc;(void)argv;
    os64_js_outcome_t saved=*quiescence_outer, probe;
    quiescence_outer->host_error=12345;
    os64_js_outcome_t before=*quiescence_outer;
    size_t allocations=attempts;
    int64_t old_step=clock_step, before_clock=monotonic;
    clock_step=1;
    check(os64_js_quiescent(quiescence_runtime,OS64_JS_ABI_ID,quiescence_outer)==OS64_JS_BUSY&&
          !memcmp(&before,quiescence_outer,sizeof(before)),"quiescence: aliased active outcome is untouched");
    check(os64_js_quiescent(quiescence_runtime,OS64_JS_ABI_ID,&probe)==OS64_JS_BUSY&&
          attempts==allocations,"quiescence: active entry refuses without allocation");
    check(monotonic==before_clock,"quiescence: active query does not sample the budget clock");
    clock_step=old_step;
    *quiescence_outer=saved;
    quiescence_calls++;
    return JS_UNDEFINED;
}
static JSValue quiescence_job(JSContext *ctx,int argc,JSValueConst *argv)
{
    return quiescence_probe(ctx,JS_UNDEFINED,argc,argv);
}
static os64_js_status_t quiescence_install(os64_js_realm_t *realm,JSContext *ctx,void *opaque)
{
    (void)realm;(void)opaque;
    quiescence_probe(ctx,JS_UNDEFINED,0,NULL);
    return OS64_JS_OK;
}
static void quiescence_cases(void)
{
    for(unsigned policy=0;policy<2;++policy){
        os64_js_realm_t *a=NULL,*b=NULL,*c=NULL;
        os64_js_runtime_t *runtime=retirement_fixture(policy,100,&a,&b,&c);
        if(!runtime)continue;
        quiescence_runtime=runtime;quiescence_calls=0;
        os64_js_outcome_t out,probe;
        quiescence_outer=&out;
        check(os64_js_quiescent(NULL,OS64_JS_ABI_ID,&probe)==OS64_JS_BAD_ARGUMENT&&
              os64_js_quiescent(runtime,NULL,&probe)==OS64_JS_BAD_ARGUMENT&&
              os64_js_quiescent(runtime,OS64_JS_ABI_ID,NULL)==OS64_JS_BAD_ARGUMENT,
              "quiescence: required inputs checked");
        check(os64_js_quiescent(runtime,"os64-js/4;quickjs=2026-06-04;lp64;value=16;limb=64;atomics=0",&probe)==OS64_JS_ABI_MISMATCH,
              "quiescence: incompatible header refused");
        size_t before=attempts;
        fail_at=attempts+1;
        check(os64_js_quiescent(runtime,OS64_JS_ABI_ID,&probe)==OS64_JS_OK&&attempts==before,
              "quiescence: idle query succeeds with allocation forbidden");
        fail_at=0;
        check(install_native(runtime,"quiescenceProbe",quiescence_probe)==1,
              "quiescence: callback installed");
        check(realm_execute(a,"quiescenceProbe();Promise.resolve().then(quiescenceProbe)",&out)==OS64_JS_OK&&
              quiescence_calls==2,"quiescence: script and checkpoint remain busy");
        check(os64_js_quiescent(runtime,OS64_JS_ABI_ID,&probe)==OS64_JS_OK,
              "quiescence: returned checkpoint is safe");
        check(os64_js_realm_task_begin(a,OS64_JS_ABI_ID,"quiescence task",&out)==OS64_JS_OK,
              "quiescence: native task opened");
        check(os64_js_quiescent(runtime,OS64_JS_ABI_ID,&probe)==OS64_JS_BUSY,
              "quiescence: open task remains busy outside engine call");
        check(os64_js_task_end(runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK&&
              os64_js_quiescent(runtime,OS64_JS_ABI_ID,&probe)==OS64_JS_OK,
              "quiescence: closed native task releases barrier");
        const char *queued="Promise.resolve().then(quiescenceProbe)";
        check(os64_js_eval(runtime,queued,strlen(queued),"quiescence queued turn",&out)==OS64_JS_OK&&
              os64_js_quiescent(runtime,OS64_JS_ABI_ID,&probe)==OS64_JS_BUSY,
              "quiescence: returned evaluation retains its unfinished turn");
        check(os64_js_drain_jobs(runtime,UINT64_MAX,&out)==OS64_JS_OK&&quiescence_calls==3&&
              os64_js_quiescent(runtime,OS64_JS_ABI_ID,&probe)==OS64_JS_OK,
              "quiescence: job drain releases barrier");
        JSContext *ctx=os64_js_realm_context(a,OS64_JS_ABI_ID,&probe);
        /* Deliberately malformed host state: reject it without pretending
         * the wrapper may execute jobs outside a budgeted turn. */
        check(JS_EnqueueJob(ctx,quiescence_job,0,NULL)==0,
              "quiescence: injected raw queue without a turn");
        check(os64_js_quiescent(runtime,OS64_JS_ABI_ID,&probe)==OS64_JS_BUSY,
              "quiescence: raw pending job prevents cleanup");
        check(JS_DiscardPendingJob(JS_GetRuntime(ctx))==1&&quiescence_calls==3&&
              os64_js_quiescent(runtime,OS64_JS_ABI_ID,&probe)==OS64_JS_OK,
              "quiescence: test removes injected work without executing it");
        os64_js_realm_t *candidate=NULL;
        check(os64_js_realm_stage(runtime,OS64_JS_ABI_ID,1,&candidate,&out)==OS64_JS_OK,
              "quiescence: staged child reserved");
        if(candidate)check(os64_js_realm_setup(candidate,OS64_JS_ABI_ID,quiescence_install,NULL,&out)==OS64_JS_OK&&
              quiescence_calls==4&&os64_js_quiescent(runtime,OS64_JS_ABI_ID,&probe)==OS64_JS_OK,
              "quiescence: installer releases barrier after return");
        check(realm_execute(a,"throw Error('ordinary sibling exception')",&out)==OS64_JS_EXCEPTION&&
              os64_js_quiescent(runtime,OS64_JS_ABI_ID,&probe)==OS64_JS_OK,
              "quiescence: catchable sibling failure does not poison cleanup");
        check(os64_js_realm_retire(b,OS64_JS_ABI_ID,&out)==OS64_JS_OK&&
              os64_js_quiescent(runtime,OS64_JS_ABI_ID,&probe)==OS64_JS_OK,
              "quiescence: retirement mark does not itself create work");
        os64_js_cancel(runtime);
        check(os64_js_quiescent(runtime,OS64_JS_ABI_ID,&probe)==OS64_JS_FAILED_RUNTIME,
              "quiescence: unobserved cancellation requires root cleanup");
        check(realm_execute(a,"0",&out)==OS64_JS_CANCELLED&&
              os64_js_quiescent(runtime,OS64_JS_ABI_ID,&probe)==OS64_JS_FAILED_RUNTIME,
              "quiescence: sticky failure never becomes safe cleanup");
        os64_js_destroy(runtime);
        check(live==0,"quiescence: fixture releases memory");

        runtime=retirement_fixture(policy,100,&a,&b,&c);
        if(!runtime)continue;
        ctx=os64_js_realm_context(a,OS64_JS_ABI_ID,&probe);
        fail_at=attempts+1;
        void *refused=js_malloc(ctx,1u<<20);
        fail_at=0;
        check(refused==NULL&&os64_js_quiescent(runtime,OS64_JS_ABI_ID,&probe)==OS64_JS_FAILED_RUNTIME,
              "quiescence: raw allocator failure requires root cleanup before observation");
        js_free(ctx,refused);
        os64_js_destroy(runtime);
        check(live==0,"quiescence: allocation-refusal fixture releases memory");

        runtime=retirement_fixture(policy,1,&a,&b,&c);
        if(!runtime)continue;
        check(realm_execute(a,"Promise.resolve().then(()=>{}).then(()=>{})",&out)==OS64_JS_LIMIT&&
              out.limit==OS64_JS_LIMIT_JOBS&&
              os64_js_quiescent(runtime,OS64_JS_ABI_ID,&probe)==OS64_JS_FAILED_RUNTIME,
              "quiescence: latched job limit requires root cleanup");
        os64_js_destroy(runtime);
        check(live==0,"quiescence: failed-turn fixture releases memory");
    }
}
