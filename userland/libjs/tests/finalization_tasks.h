#ifndef OS64_JS_FINALIZATION_TASK_CASES_H
#define OS64_JS_FINALIZATION_TASK_CASES_H

static unsigned finalization_task_markers;
static JSValue finalization_task_marker(JSContext *ctx, int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    finalization_task_markers++;
    return JS_UNDEFINED;
}
static JSValue finalization_task_callback(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    check(finalization_settings->mode == 8 || finalization_task_markers == finalization_settings->calls,
          "cleanup task: previous task's microtasks precede the next cleanup");
    JSValue result = finalization_probe(ctx, self, argc, argv);
    os64_js_outcome_t out;
    check(os64_js_run_finalization_task(finalization_settings->runtime, OS64_JS_ABI_ID, &out) == OS64_JS_BUSY,
          "cleanup task: recursive dispatch refuses under the active guard");
    if (!JS_IsException(result))
    for(unsigned i=0;i<(finalization_settings->mode==20?100u:1u);i++)
        check(JS_EnqueueJob(ctx, finalization_task_marker, 0, NULL) == 0,
              "cleanup task: callback queues a microtask");
    return result;
}
static bool finalization_tasks_open(FinalizationSettings *f, int policy, unsigned mode)
{
    if (!finalization_open(f, policy, mode)) return false;
    os64_js_outcome_t out;
    check(os64_js_enable_finalization_tasks(f->runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK,
          "cleanup task: scheduler configured before execution");
    finalization_task_markers = 0;
    JSValue global = JS_GetGlobalObject(f->ctx[2]);
    check(JS_SetPropertyStr(f->ctx[2], global, "cleanup",
        JS_NewCFunction(f->ctx[2], finalization_task_callback, "cleanup", 1)) >= 0,
        "cleanup task: guarded native callback installed");
    JS_FreeValue(f->ctx[2], global);
    return true;
}
static void finalization_tasks_collect(FinalizationSettings *f)
{
    JS_RunGC(JS_GetRuntime(f->ctx[0]));
    JS_RunGC(JS_GetRuntime(f->ctx[0]));
}
static os64_js_status_t finalization_task_install(os64_js_realm_t *realm, JSContext *ctx, void *opaque)
{
    (void)realm;
    const char *source = "globalThis.cleaned=0;globalThis.reg=new FinalizationRegistry(()=>cleaned++);"
        "let t={};t.self=t;reg.register(t,3);t=null;Promise.resolve().then(()=>globalThis.initialized=true)";
    JSValue value = JS_Eval(ctx,source,os64_strlen(source),"candidate-cleanup",JS_EVAL_TYPE_GLOBAL);
    bool failed = JS_IsException(value);
    JS_FreeValue(ctx,value);
    if (failed) return OS64_JS_EXCEPTION;
    JS_RunGC(JS_GetRuntime(ctx)); JS_RunGC(JS_GetRuntime(ctx));
    return opaque ? OS64_JS_BAD_ARGUMENT : OS64_JS_OK;
}
static void finalization_task_staging_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++)
    for(unsigned abort=0;abort<2;abort++) {
        FinalizationSettings f;
        if(!finalization_tasks_open(&f,policy,0)){finalization_close(&f);continue;}
        os64_js_outcome_t out; os64_js_realm_t *candidate=NULL;
        check(realm_execute(f.realm[0],"captureRecord();globalThis.reg=new D.FinalizationRegistry(C.cleanup);"
              "let t={};t.self=t;reg.register(t,73);t=null",&out)==OS64_JS_OK,"cleanup transaction: sibling prepared");
        finalization_tasks_collect(&f);
        check(os64_js_realm_stage(f.runtime,OS64_JS_ABI_ID,1024,&candidate,&out)==OS64_JS_OK,
              "cleanup transaction: candidate staged beside pending sibling task");
        if(!candidate){finalization_close(&f);continue;}
        check(os64_js_realm_setup(candidate,OS64_JS_ABI_ID,finalization_task_install,
              abort?&f:NULL,&out)==(abort?OS64_JS_BAD_ARGUMENT:OS64_JS_OK),
              "cleanup transaction: selected installation result preserved");
        check(f.calls==0,"cleanup transaction: installation does not drain sibling cleanup");
        check(os64_js_run_finalization_task(f.runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK&&f.calls==1,
              "cleanup transaction: sibling task survives installation or rollback");
        if(abort) {
            check(!os64_js_finalization_task_pending(f.runtime,OS64_JS_ABI_ID),
                  "cleanup transaction: rollback discarded candidate task lane");
        } else {
            check(os64_js_run_finalization_task(f.runtime,OS64_JS_ABI_ID,&out)==OS64_JS_BUSY,
                  "cleanup transaction: unpublished candidate task waits for activation");
            check(os64_js_realm_activate(candidate,OS64_JS_ABI_ID,&out)==OS64_JS_OK,
                  "cleanup transaction: deferred cleanup does not block activation");
            check(os64_js_run_finalization_task(f.runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,
                  "cleanup transaction: candidate cleanup runs after activation");
            check(realm_execute(candidate,"if(cleaned!==1||!initialized)throw Error('candidate ordering')",&out)==OS64_JS_OK,
                  "cleanup transaction: installer microtask and deferred cleanup both execute");
        }
        finalization_close(&f);
    }
}

static JSClassID finalization_release_class;
static unsigned finalization_releases;
static void finalization_release(JSRuntime *rt, JSValue value)
{
    (void)rt; (void)value;
    finalization_releases++;
    check(JS_EnqueueJob(finalization_settings->ctx[2],finalization_task_marker,0,NULL)==0,
          "cleanup discard: native finalizer queues independent work");
}
static void finalization_task_release_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++)
    for(unsigned destroy=0;destroy<2;destroy++) {
        FinalizationSettings f;
        if(!finalization_tasks_open(&f,policy,0)){finalization_close(&f);continue;}
        os64_js_outcome_t out; finalization_releases=0;
        finalization_release_class=os64_js_class_id(&finalization_release_class);
        JSClassDef def={.class_name="cleanup release",.finalizer=finalization_release};
        check(JS_NewClass(JS_GetRuntime(f.ctx[0]),finalization_release_class,&def)==0,"cleanup discard: finalizer registered");
        JSValue global=JS_GetGlobalObject(f.ctx[0]);
        check(JS_SetPropertyStr(f.ctx[0],global,"held",JS_NewObjectClass(f.ctx[0],finalization_release_class))>=0,
              "cleanup discard: held object installed");
        JS_FreeValue(f.ctx[0],global);
        check(realm_execute(f.realm[0],"captureRecord();globalThis.reg=new D.FinalizationRegistry(C.cleanup);"
              "let t={};t.self=t;reg.register(t,held);t=null;held=null",&out)==OS64_JS_OK,
              "cleanup discard: native held value registered");
        finalization_tasks_collect(&f);
        check(realm_execute(f.realm[0],"reg=null",&out)==OS64_JS_OK,"cleanup discard: task holds the registry");
        check(os64_js_realm_retire(f.realm[0],OS64_JS_ABI_ID,&out)==OS64_JS_OK,"cleanup discard: captured incumbent retires");
        if(destroy) {
            finalization_close(&f);
            check(finalization_releases==1 && finalization_task_markers==0,
                  "cleanup destruction: native finalizer enqueue is discarded without running its job");
            continue;
        }
        check(os64_js_run_finalization_task(f.runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK &&
              out.jobs_discarded==1 && out.jobs_executed==1 && finalization_task_markers==1 && !f.calls && !out.jobs_pending,
              "cleanup discard: finalizer work shares a bounded task and drains before return");
        finalization_close(&f);
    }
}

static void finalization_task_budget_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++) {
        FinalizationSettings f;
        if(!finalization_tasks_open(&f,policy,20)){finalization_close(&f);continue;}
        os64_js_outcome_t out;
        check(fixture_config().limits.jobs_per_turn==100,"cleanup budget: fixture cap established");
        check(realm_execute(f.realm[0],"captureRecord();globalThis.reg=new D.FinalizationRegistry(C.cleanup);"
              "let t={};t.self=t;reg.register(t,73);t=null",&out)==OS64_JS_OK,"cleanup budget: target prepared");
        finalization_tasks_collect(&f);
        check(os64_js_run_finalization_task(f.runtime,OS64_JS_ABI_ID,&out)==OS64_JS_LIMIT &&
              out.limit==OS64_JS_LIMIT_JOBS && out.jobs_executed==100 && out.jobs_pending &&
              finalization_task_markers==99 && f.calls==1,
              "cleanup budget: cleanup and its microtasks share one job cap");
        check(os64_js_run_finalization_task(f.runtime,OS64_JS_ABI_ID,&out)==OS64_JS_FAILED_RUNTIME,
              "cleanup budget: another dispatch cannot renew failed runtime");
        finalization_close(&f);
    }
}

static void finalization_task_cases(void)
{
    finalization_task_staging_cases();
    finalization_task_release_cases();
    finalization_task_budget_cases();
    for (int policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; policy++)
    for (unsigned mode = 0; mode < 12; mode++) {
        FinalizationSettings f;
        unsigned callback_mode = mode == 7 ? 8 : mode == 8 ? 9 : mode == 6 ? 6 : 0;
        if (!finalization_tasks_open(&f, policy, callback_mode)) { finalization_close(&f); continue; }
        os64_js_outcome_t out;
        RealmReports executable = {.context=f.ctx[2]}, registry = {.context=f.ctx[3]};
        realm_host(f.realm[2], &executable); realm_host(f.realm[3], &registry);
        check(realm_execute(f.realm[1], "globalThis.pair=Proxy.revocable(C.cleanup,{});globalThis.handler=pair.proxy", &out)==OS64_JS_OK,
              "cleanup task: separate proxy realm installed");
        check(realm_execute(f.realm[0],
            "captureRecord();globalThis.registry=new D.FinalizationRegistry(B.handler);"
            "globalThis.token={};let target={};target.self=target;registry.register(target,73,token);target=null;"
            "globalThis.second={};second.self=second;registry.register(second,73);second=null", &out)==OS64_JS_OK,
              "cleanup task: registry and two cyclic targets constructed");
        finalization_tasks_collect(&f);
        check(!JS_IsJobPending(JS_GetRuntime(f.ctx[0])) &&
              os64_js_finalization_task_pending(f.runtime, OS64_JS_ABI_ID) && f.calls == 0,
              "cleanup task: GC schedules a separate queue without author invocation");
        JSJobInfo info; JSContext *entry=NULL, *incumbent=NULL, *report=NULL;
        check(JS_PeekFinalizationTask(JS_GetRuntime(f.ctx[0]),NULL,&info) && info.activity==f.ctx[3] && info.owner==f.ctx[2] &&
              JS_PeekFinalizationTaskSettings(JS_GetRuntime(f.ctx[0]),NULL,&entry,&incumbent,&report) &&
              entry==f.ctx[2] && incumbent==f.ctx[0] && report==f.ctx[3],
              "cleanup task: registry global, executable and captured settings stay distinct");
        check(os64_js_quiescent(f.runtime, OS64_JS_ABI_ID, &out) == OS64_JS_BUSY,
              "cleanup task: pending cleanup prevents quiescent recovery admission");
        check(os64_js_realm_task_begin(f.realm[0],OS64_JS_ABI_ID,"intervening task",&out)==OS64_JS_OK,
              "cleanup task: pending finalization does not block ordinary tasks");
        check(os64_js_run_finalization_task(f.runtime,OS64_JS_ABI_ID,&out)==OS64_JS_BUSY,
              "cleanup task: dispatch cannot borrow an open host task");
        check(os64_js_checkpoint(f.runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK && f.calls==0,
              "cleanup task: checkpoint does not dispatch cleanup");
        check(os64_js_task_end(f.runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK && f.calls==0,
              "cleanup task: ordinary task completion leaves cleanup queued");
        if (mode == 1)
            check(realm_execute(f.realm[0],"if(!registry.unregister(token))throw Error('missing collected cell')",&out)==OS64_JS_OK,
                  "cleanup task: unregister cancels a collected cell before delivery");
        if (mode == 2) {
            check(realm_execute(f.realm[0],"registry=null",&out)==OS64_JS_OK,"cleanup task: script releases registry");
            finalization_tasks_collect(&f);
        }
        if (mode >= 3 && mode <= 6) {
            unsigned index = mode==3 ? 0 : mode==4 ? 2 : mode==5 ? 3 : 1;
            check(os64_js_realm_retire(f.realm[index],OS64_JS_ABI_ID,&out)==OS64_JS_OK,"cleanup task: selected role retired");
        }
        if (mode == 9)
            check(realm_execute(f.realm[1],"pair.revoke()",&out)==OS64_JS_OK,"cleanup task: callback revoked after GC");
        if (mode == 10) {
            check(os64_js_eval(f.runtime,"Promise.resolve().then(()=>{})",30,"queued-before-cleanup",&out)==OS64_JS_OK,
                  "cleanup task: ordinary evaluation leaves pending microtasks");
            check(os64_js_run_finalization_task(f.runtime,OS64_JS_ABI_ID,&out)==OS64_JS_BUSY,
                  "cleanup task: dispatch cannot bypass unfinished microtasks");
            check(os64_js_drain_jobs(f.runtime,64,&out)==OS64_JS_OK,"cleanup task: preceding microtasks drain");
        }
        if (mode == 11) { finalization_close(&f); continue; }
        os64_js_status_t expected = mode==7 || mode==9 ? OS64_JS_EXCEPTION : mode==8 ? OS64_JS_CANCELLED : OS64_JS_OK;
        check(os64_js_run_finalization_task(f.runtime,OS64_JS_ABI_ID,&out)==expected,
              "cleanup task: first task reports its own completion");
        check(os64_js_finalization_task_pending(f.runtime,OS64_JS_ABI_ID),"cleanup task: one call leaves the second task queued");
        if (mode != 8) {
            check(os64_js_run_finalization_task(f.runtime,OS64_JS_ABI_ID,&out)==expected,
                  "cleanup task: second task has a separate host dispatch");
            check(!os64_js_finalization_task_pending(f.runtime,OS64_JS_ABI_ID),"cleanup task: both entries consumed");
            unsigned calls = mode>=3&&mode<=5 ? 0 : mode==9 ? 0 : mode==1 ? 1 : 2;
            check(f.calls==calls,"cleanup task: lifecycle and unregister delivery counts");
            check(finalization_task_markers==(mode==7?0:calls),"cleanup task: each successful cleanup drains its own microtasks");
            check(executable.exceptions==0 && registry.exceptions==((mode==7||mode==9)?2u:0u),
                  "cleanup task: errors report through registry global");
            check(os64_js_run_finalization_task(f.runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,
                  "cleanup task: empty dispatch is harmless");
        }
        finalization_close(&f);
    }
}
#endif
