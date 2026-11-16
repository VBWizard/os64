#ifndef OS64_JS_RETIREMENT_CASES_H
#define OS64_JS_RETIREMENT_CASES_H
/* Trusted host fixtures. DOM/provider departure is a separate binding layer. */
static os64_js_realm_t *retirement_target;
static os64_js_outcome_t *retirement_outer;
static JSValue retirement_mark(JSContext *context, JSValueConst self,
                                int argc, JSValueConst *argv)
{
    (void)context; (void)self; (void)argc; (void)argv;
    os64_js_outcome_t out, before = *retirement_outer;
    check(os64_js_realm_retire(retirement_target, OS64_JS_ABI_ID, retirement_outer) == OS64_JS_BUSY &&
          os64_memcmp(&before, retirement_outer, sizeof(before)) == 0,
          "retirement rejects an aliased outer outcome without writing it");
    check(os64_js_realm_retire(retirement_target, OS64_JS_ABI_ID, &out) == OS64_JS_OK &&
          os64_js_realm_is_retired(retirement_target), "retirement marks during an active cross-realm stack");
    check(os64_memcmp(&before, retirement_outer, sizeof(before)) == 0,
          "retirement leaves enclosing outcome intact");
    return JS_UNDEFINED;
}

static os64_js_runtime_t *retirement_fixture(unsigned policy, uint64_t jobs,
                                            os64_js_realm_t **a, os64_js_realm_t **b,
                                            os64_js_realm_t **c)
{
    os64_js_config_t config = fixture_config();
    config.limits.jobs_per_turn = jobs;
    os64_js_outcome_t out;
    os64_js_runtime_t *runtime = NULL;
    check(os64_js_create_with_teardown(&config, policy ? OS64_JS_TEARDOWN_RECLAIM : OS64_JS_TEARDOWN_FATAL,
          OS64_JS_ABI_ID, &runtime, &out) == OS64_JS_OK, "retirement runtime created");
    if (!runtime) return NULL;
    *a = os64_js_primary_realm(runtime, OS64_JS_ABI_ID, &out);
    *b = NULL; *c = NULL;
    check(os64_js_realm_create(runtime, OS64_JS_ABI_ID, b, &out) == OS64_JS_OK &&
          os64_js_realm_create(runtime, OS64_JS_ABI_ID, c, &out) == OS64_JS_OK,
          "retirement sibling realms created");
    if (!*b || !*c) { os64_js_destroy(runtime); return NULL; }
    check(realm_execute(*a, "globalThis.log=[];globalThis.PA=Promise;globalThis.addA=()=>log.push('A');"
          "globalThis.pA=new Promise(r=>globalThis.finishA=r)", &out) == OS64_JS_OK,
          "retirement primary fixtures installed");
    JSContext *ac = os64_js_realm_context(*a, OS64_JS_ABI_ID, &out);
    JSContext *bc = os64_js_realm_context(*b, OS64_JS_ABI_ID, &out);
    JSContext *cc = os64_js_realm_context(*c, OS64_JS_ABI_ID, &out);
    realm_share(ac, bc, "log"); realm_share(ac, cc, "log"); realm_share(ac, bc, "PA");
    check(realm_execute(*b, "globalThis.addB=()=>log.push('B');globalThis.boundB=addB.bind(null);"
          "globalThis.proxyB=new Proxy(addB,{});globalThis.thenB={then(r){log.push('thenB');r(1)}};"
          "globalThis.pB=new Promise(r=>globalThis.finishB=r);"
          "globalThis.lateB=()=>Promise.resolve().then(addB);"
          "globalThis.workB=async()=>{await pB;log.push('asyncB')};"
          "globalThis.genB=async function*(){await pB;log.push('genB');yield 1};"
          "globalThis.SpeciesB=function(executor){return new PA((r,j)=>executor("
          "v=>{log.push('resolveB');r(v)},e=>{log.push('rejectB');j(e)}))}", &out) == OS64_JS_OK,
          "retirement child fixtures installed");
    realm_share(bc, cc, "pB");
    check(realm_execute(*c, "globalThis.addC=()=>log.push('C');globalThis.thenC={then(r){log.push('thenC');r(1)}};"
          "globalThis.workC=async()=>{await pB;log.push('asyncC')}", &out) == OS64_JS_OK,
          "retirement live sibling fixtures installed");
    const char *bn[] = {"addB","boundB","proxyB","thenB","pB","finishB","lateB","workB","genB","SpeciesB"};
    for (unsigned i=0; i<sizeof(bn)/sizeof(bn[0]); i++) realm_share(bc, ac, bn[i]);
    realm_share(cc, ac, "addC"); realm_share(cc, ac, "thenC"); realm_share(cc, ac, "workC");
    return runtime;
}

static void retirement_queue_cases(unsigned policy)
{
    os64_js_realm_t *a, *b, *c;
    os64_js_runtime_t *runtime = retirement_fixture(policy, 100, &a, &b, &c);
    if (!runtime) return;
    os64_js_outcome_t out;
    const char *source = "pB.then(addA);pA.then(addB);pA.then(addC);pA.then().then(addC);finishB(1);finishA(2);"
        "Promise.resolve().then(boundB);Promise.resolve().then(proxyB);Promise.resolve(thenB);Promise.resolve(thenC)";
    check(os64_js_eval(runtime, source, os64_strlen(source), "retirement-queue", &out) == OS64_JS_OK && out.jobs_pending,
          "mixed jobs remain queued before retirement");
    size_t bytes = os64_js_heap_bytes(runtime);
    check(os64_js_realm_retire(b, "wrong-abi", &out) == OS64_JS_ABI_MISMATCH && !os64_js_realm_is_retired(b),
          "retirement ABI mismatch preserves active state");
    check(os64_js_realm_retire(b, OS64_JS_ABI_ID, &out) == OS64_JS_OK &&
          os64_js_realm_retire(b, OS64_JS_ABI_ID, &out) == OS64_JS_OK && os64_js_heap_bytes(runtime) == bytes,
          "retirement is idempotent and does not free retained state");
    check(os64_js_drain_jobs(runtime, 100, &out) == OS64_JS_OK && out.jobs_discarded == 4 && out.jobs_executed == 5,
          "handler and then realms select four dead jobs without charging execution");
    check(realm_execute(a, "if(log.join()!=='A,C,thenC,C')throw log;log.length=0;addB();lateB();Promise.resolve().then(addC)", &out) == OS64_JS_OK &&
          out.jobs_discarded == 1, "retained ordinary call runs while its new scheduled job is suppressed");
    check(realm_execute(a, "if(log.join()!=='B,C')throw log", &out) == OS64_JS_OK,
          "post-retirement direct call and live delivery preserve FIFO effects");
    check(realm_execute(b, "throw 1", &out) == OS64_JS_REALM_RETIRED &&
          os64_js_realm_task_begin(b, OS64_JS_ABI_ID, "dead-task", &out) == OS64_JS_REALM_RETIRED &&
          os64_js_realm_context(b, OS64_JS_ABI_ID, &out) != NULL,
          "new retired entries refuse while context access remains valid");
    os64_js_module_t *module = NULL;
    check(os64_js_realm_module_compile(b, OS64_JS_ABI_ID, "export{}", 8, "dead", NULL, &module, &out) == OS64_JS_REALM_RETIRED && !module,
          "retired module compilation refuses before allocation");
    JSContext *ac = os64_js_realm_context(a, OS64_JS_ABI_ID, &out);
    JSValue func = realm_property(ac, "addB");
    check(os64_js_task_begin(runtime, OS64_JS_ABI_ID, "host-callback", &out) == OS64_JS_OK &&
          os64_js_realm_call(a, OS64_JS_ABI_ID, func, JS_UNDEFINED, 0, NULL, NULL, &out) == OS64_JS_REALM_RETIRED &&
          os64_js_task_end(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK,
          "a live host entry cannot schedule a retired function by changing its context");
    JS_FreeValue(ac, func);
    check(realm_execute(c, "true", &out) == OS64_JS_OK, "ordinary retirement preserves third realm");
    os64_js_destroy(runtime);
}

static void retirement_continuation_cases(unsigned policy)
{
    os64_js_realm_t *a, *b, *c;
    os64_js_runtime_t *runtime = retirement_fixture(policy, 100, &a, &b, &c);
    if (!runtime) return;
    os64_js_outcome_t out;
    check(realm_execute(a, "workB();workC();genB().next();"
          "globalThis.rejected=new Promise((r,j)=>globalThis.rejectA=j);"
          "pA.constructor=rejected.constructor={[Symbol.species]:SpeciesB};"
          "pA.then().then(addA);rejected.then().catch(addC);"
          "let x=Proxy.revocable(addB,{});x.revoke();globalThis.revoked=x.proxy", &out) == OS64_JS_OK,
          "cross-realm async and neutral custom-capability work prepared");
    check(os64_js_realm_retire(b, OS64_JS_ABI_ID, &out) == OS64_JS_OK, "continuation owner retired");
    check(realm_execute(a, "finishB(1);finishA(2);rejectA(3);"
          "Promise.resolve().then(revoked).catch(()=>log.push('revoked'));"
          "Promise.resolve({then:revoked}).catch(()=>log.push('revokedThen'))", &out) == OS64_JS_OK && out.jobs_discarded == 2,
          "async function/generator code owners suppressed while neutral capabilities and proxy fallback run");
    check(realm_execute(a, "if(log.includes('asyncB')||log.includes('genB')||"
          "['asyncC','resolveB','rejectB','A','C','revoked','revokedThen'].some(x=>!log.includes(x)))throw log", &out) == OS64_JS_OK,
          "live continuation and custom species settlement survive retired wrappers");
    os64_js_destroy(runtime);
}

static unsigned retirement_native_hits;
static JSValue retirement_native_job(JSContext *ctx, int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    retirement_native_hits++;
    return JS_UNDEFINED;
}
static void retirement_budget_cases(unsigned policy, bool checkpoint_mode)
{
    os64_js_realm_t *a, *b, *c;
    os64_js_runtime_t *runtime = retirement_fixture(policy, 1, &a, &b, &c);
    if (!runtime) return;
    os64_js_outcome_t out;
    JSContext *ac=os64_js_realm_context(a,OS64_JS_ABI_ID,&out), *bc=os64_js_realm_context(b,OS64_JS_ABI_ID,&out);
    retirement_native_hits=0;
    if (checkpoint_mode) {
        check(os64_js_task_begin(runtime,OS64_JS_ABI_ID,"discard-checkpoint",&out)==OS64_JS_OK,
              "discard checkpoint task opened");
        check(JS_EnqueueJob(ac,retirement_native_job,0,NULL)==0,"live native job queued");
    } else {
        check(os64_js_eval(runtime,"Promise.resolve().then(addA)",sizeof("Promise.resolve().then(addA)")-1,"discard-slice",&out)==OS64_JS_OK,
              "manual discard turn opened with live job");
    }
    for(unsigned i=0;i<5;i++) check(JS_EnqueueJob(bc,retirement_native_job,0,NULL)==0,"dead native job queued");
    check(os64_js_realm_retire(b,OS64_JS_ABI_ID,&out)==OS64_JS_OK,"budget child retired");
    if(checkpoint_mode) {
        check(os64_js_task_end(runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK && retirement_native_hits==1 && out.jobs_executed==1 && out.jobs_discarded==5,
              "checkpoint discards dead heads even after executing its job cap");
    } else {
        for(unsigned i=0;i<6;i++) {
            os64_js_status_t result=os64_js_drain_jobs(runtime,1,&out);
            check(result==(i<5?OS64_JS_MORE_JOBS:OS64_JS_OK) && out.jobs_executed==1 && out.jobs_discarded==i,
                  "manual slices bound discarded visits without charging live execution");
        }
        check(retirement_native_hits==0,"discarded native jobs have no side effects");
    }
    check(realm_execute(c,"true",&out)==OS64_JS_OK,"dead queue exhaustion closes a healthy turn");
    os64_js_destroy(runtime);
}

static void retirement_stack_cases(unsigned policy)
{
    os64_js_realm_t *a,*b,*c;
    os64_js_runtime_t *runtime=retirement_fixture(policy,100,&a,&b,&c);
    if(!runtime)return;
    os64_js_outcome_t out;
    retirement_target=b;retirement_outer=&out;
    check(install_native(runtime,"retireB",retirement_mark)==1,"retirement hook installed");
    JSContext *ac=os64_js_realm_context(a,OS64_JS_ABI_ID,&out),*bc=os64_js_realm_context(b,OS64_JS_ABI_ID,&out);
    realm_share(ac,bc,"retireB");
    check(realm_execute(b,"globalThis.retiringCall=()=>{log.push('before');retireB();log.push('after');lateB()}",&out)==OS64_JS_OK,
          "retiring function exported");
    realm_share(bc,ac,"retiringCall");
    check(realm_execute(a,"retiringCall();log.push('parent')",&out)==OS64_JS_OK && out.jobs_discarded==1,
          "retirement preserves active stack unwind and suppresses its later job");
    check(realm_execute(a,"if(log.join()!=='before,after,parent')throw log",&out)==OS64_JS_OK,
          "retired stack reaches parent continuation without cancellation");
    os64_js_cancel(runtime);
    check(os64_js_realm_retire(b,OS64_JS_ABI_ID,&out)==OS64_JS_OK,
          "idempotent retirement remains available for native failure cleanup");
    os64_js_destroy(runtime);retirement_target=NULL;retirement_outer=NULL;
}

static void retirement_module_cases(unsigned policy)
{
    os64_js_realm_t *a, *b, *c;
    os64_js_runtime_t *runtime = retirement_fixture(policy, 100, &a, &b, &c);
    if (!runtime) return;
    os64_js_outcome_t out;
    os64_js_module_t *bm = NULL, *cm = NULL;
    const char *bs = "await pB;log.push('moduleB');throw Error('obsolete')";
    const char *cs = "await pB;log.push('moduleC')";
    check(realm_execute(a,"globalThis.settle=()=>finishB(1)",&out)==OS64_JS_OK &&
          os64_js_realm_module_compile(b,OS64_JS_ABI_ID,bs,os64_strlen(bs),"b.mjs",NULL,&bm,&out)==OS64_JS_OK &&
          os64_js_realm_module_compile(c,OS64_JS_ABI_ID,cs,os64_strlen(cs),"c.mjs",NULL,&cm,&out)==OS64_JS_OK,
          "pending module continuation fixtures compiled");
    check(os64_js_task_begin(runtime,OS64_JS_ABI_ID,"module-retirement",&out)==OS64_JS_OK &&
          os64_js_realm_module_run(b,OS64_JS_ABI_ID,bm,&out)==OS64_JS_OK &&
          os64_js_realm_module_run(c,OS64_JS_ABI_ID,cm,&out)==OS64_JS_OK &&
          os64_js_checkpoint(runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,
          "both modules suspend under one task");
    check(os64_js_realm_retire(b,OS64_JS_ABI_ID,&out)==OS64_JS_OK,"pending module owner retired");
    JSContext *ac=os64_js_realm_context(a,OS64_JS_ABI_ID,&out);
    JSValue settle=realm_property(ac,"settle");
    check(os64_js_realm_call(a,OS64_JS_ABI_ID,settle,JS_UNDEFINED,0,NULL,NULL,&out)==OS64_JS_OK &&
          os64_js_task_end(runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK && out.jobs_discarded>0,
          "live module completes while retired module continuation is discarded");
    JS_FreeValue(ac,settle);
    check(realm_execute(a,"if(log.join()!=='moduleC')throw log",&out)==OS64_JS_OK,
          "retired module emits neither side effects nor late failure");
    check(os64_js_realm_module_release(b,OS64_JS_ABI_ID,bm,&out)==OS64_JS_OK &&
          os64_js_realm_module_release(c,OS64_JS_ABI_ID,cm,&out)==OS64_JS_OK,
          "module cleanup remains available after retirement");
    os64_js_destroy(runtime);
}

static void retirement_registry_cases(unsigned policy)
{
    os64_js_realm_t *a,*b,*c;
    os64_js_runtime_t *runtime=retirement_fixture(policy,100,&a,&b,&c);
    if(!runtime)return;
    os64_js_outcome_t out;
    JSContext *ac=os64_js_realm_context(a,OS64_JS_ABI_ID,&out),*bc=os64_js_realm_context(b,OS64_JS_ABI_ID,&out),*cc=os64_js_realm_context(c,OS64_JS_ABI_ID,&out);
    /* Cycles make cleanup occur at the explicit collection, after marking. */
    JS_SetGCThreshold(JS_GetRuntime(ac),(size_t)-1);
    realm_share(ac,bc,"addC");realm_share(ac,cc,"addB");
    check(realm_execute(b,"globalThis.registryB=new FinalizationRegistry(addC);"
          "(()=>{let x={};x.self=x;registryB.register(x,1)})()",&out)==OS64_JS_OK &&
          realm_execute(c,"globalThis.registryC=new FinalizationRegistry(addB);"
          "globalThis.registryLive=new FinalizationRegistry(addC);"
          "(()=>{let x={};x.self=x;registryC.register(x,2);let y={};y.self=y;registryLive.register(y,3)})()",&out)==OS64_JS_OK,
          "registry and callback ownership fixtures installed");
    check(os64_js_realm_retire(b,OS64_JS_ABI_ID,&out)==OS64_JS_OK &&
          os64_js_task_begin(runtime,OS64_JS_ABI_ID,"registry-retirement",&out)==OS64_JS_OK,
          "registry cleanup turn opened after retirement");
    JS_RunGC(JS_GetRuntime(ac));
    /* Weak-target processing precedes cycle collection in this engine. */
    JS_RunGC(JS_GetRuntime(ac));
    check(os64_js_task_end(runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK && out.jobs_discarded==2,
          "registry cleanup gates both registry activity and callback realm");
    check(realm_execute(a,"if(log.join()!=='C')throw log",&out)==OS64_JS_OK,
          "live registry cleanup survives retired neighboring registries");
    os64_js_destroy(runtime);
}

static void retirement_reporting_cases(unsigned policy)
{
    os64_js_realm_t *a,*b,*c;
    os64_js_runtime_t *runtime=retirement_fixture(policy,100,&a,&b,&c);
    if(!runtime)return;
    os64_js_outcome_t out;
    JSContext *bc=os64_js_realm_context(b,OS64_JS_ABI_ID,&out),*cc=os64_js_realm_context(c,OS64_JS_ABI_ID,&out);
    RealmReports br={.context=bc},cr={.context=cc};
    realm_host(b,&br);realm_host(c,&cr);
    check(realm_execute(b,"globalThis.rejectB=()=>Promise.reject(43);Promise.reject(41)",&out)==OS64_JS_OK &&
          realm_execute(c,"Promise.reject(42)",&out)==OS64_JS_OK,"independent hosted rejection records retained");
    JSContext *ac=os64_js_realm_context(a,OS64_JS_ABI_ID,&out);
    realm_share(bc,ac,"rejectB");
    check(os64_js_realm_retire(b,OS64_JS_ABI_ID,&out)==OS64_JS_OK &&
          os64_js_realm_set_script_host(b,OS64_JS_ABI_ID,NULL,&out)==OS64_JS_OK,
          "retired report provider can detach at host safe point");
    check(realm_execute(a,"rejectB()",&out)==OS64_JS_OK && br.rejected==1 && cr.rejected==1 && realm_take(c,cc,42),
          "obsolete rejection reports are removed without losing live sibling records");
    check(os64_js_realm_set_script_host(b,OS64_JS_ABI_ID,&(os64_js_script_host_t){0},&out)==OS64_JS_REALM_RETIRED,
          "retired report provider cannot be rearmed");
    os64_js_destroy(runtime);
}

static void retirement_running_job_budget(unsigned policy)
{
    os64_js_realm_t *a,*b,*c;
    os64_js_runtime_t *runtime=retirement_fixture(policy,1,&a,&b,&c);
    if(!runtime)return;
    os64_js_outcome_t out;
    retirement_target=b;retirement_outer=&out;
    check(install_native(runtime,"retireB",retirement_mark)==1,"job retirement hook installed");
    const char *source="Promise.resolve().then(()=>retireB());Promise.resolve().then(()=>log.push('second'))";
    check(realm_execute(a,source,&out)==OS64_JS_LIMIT && out.limit==OS64_JS_LIMIT_JOBS && out.jobs_executed==1,
          "mark inside a running job cannot renew the enclosing job cap");
    os64_js_destroy(runtime);retirement_target=NULL;retirement_outer=NULL;
}

static unsigned retirement_native_hits;
static JSValue retirement_native_step(JSContext *context, JSValueConst self,
                                      int argc, JSValueConst *argv, int magic, JSValue *data)
{
    (void)context; (void)self; (void)argc; (void)argv; (void)magic; (void)data;
    retirement_native_hits++;
    return JS_UNDEFINED;
}

static void retirement_native_cases(unsigned policy, bool capped)
{
    os64_js_realm_t *a, *b, *c;
    os64_js_runtime_t *runtime = retirement_fixture(policy, capped ? 1 : 100, &a, &b, &c);
    if (!runtime) return;
    os64_js_outcome_t out;
    JSContext *ac = os64_js_realm_context(a, OS64_JS_ABI_ID, &out);
    JSContext *bc = os64_js_realm_context(b, OS64_JS_ABI_ID, &out);
    check(os64_js_realm_task_begin(a, OS64_JS_ABI_ID, "native-continuations", &out) == OS64_JS_OK,
          "native continuation fixture entry opens");
    JSValue global = JS_GetGlobalObject(ac);
    JSValue neutral = JS_NewCFunctionDataNeutral(bc, retirement_native_step, 1, 0, 0, NULL);
    check(!JS_IsException(neutral) && JS_GetFunctionRealm(ac, neutral) == bc,
          "neutral continuation retains ordinary function realm identity");
    check(JS_SetPropertyStr(ac, global, "neutral", neutral) >= 0 &&
          JS_SetPropertyStr(ac, global, "owned", JS_NewCFunctionData(bc, retirement_native_step, 1, 0, 0, NULL)) >= 0,
          "neutral and realm-owned native functions installed");
    JS_FreeValue(ac, global);
    check(os64_js_task_end(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK,
          "native continuation fixture entry closes");
    check(os64_js_realm_retire(b, OS64_JS_ABI_ID, &out) == OS64_JS_OK, "native creator retired");
    retirement_native_hits = 0;
    os64_js_status_t status = realm_execute(a,
        "Promise.resolve().then(owned);Promise.resolve().then(neutral);"
        "Promise.resolve().then(neutral.bind(null));Promise.resolve().then(new Proxy(neutral,{}))", &out);
    if (capped) {
        check(status == OS64_JS_LIMIT && out.limit == OS64_JS_LIMIT_JOBS &&
              retirement_native_hits == 1 && out.jobs_executed == 1 && out.jobs_discarded == 1,
              "neutral continuations consume enclosing job budget after dead owner discard");
    } else {
        check(status == OS64_JS_OK && retirement_native_hits == 3 &&
              out.jobs_executed == 3 && out.jobs_discarded == 1,
              "neutral direct bound and proxy continuations survive without reviving ordinary native callbacks");
    }
    os64_js_destroy(runtime);
}

static void retirement_cases(void)
{
    for(unsigned policy=0;policy<2;policy++) {
        retirement_native_cases(policy, false);
        retirement_native_cases(policy, true);
        retirement_queue_cases(policy);
        retirement_continuation_cases(policy);
        retirement_budget_cases(policy,false);
        retirement_budget_cases(policy,true);
        retirement_stack_cases(policy);
        retirement_module_cases(policy);
        retirement_registry_cases(policy);
        retirement_reporting_cases(policy);
        retirement_running_job_budget(policy);
    }
}
#endif
