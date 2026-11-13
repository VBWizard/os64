#ifndef OS64_JS_STAGING_CASES_H
#define OS64_JS_STAGING_CASES_H
/* Native fixtures deliberately observe private setup without publishing it. */
static unsigned staging_jobs, staging_sibling_jobs;
static os64_js_runtime_t *staging_runtime;
static os64_js_realm_t *staging_child;
static os64_js_outcome_t *staging_outer;
static unsigned staging_mode;
static RealmReports staging_reports;

static JSValue staging_neutral_job(JSContext *context, int argc, JSValueConst *argv)
{
    (void)argc;
    staging_jobs++;
    if (JS_VALUE_GET_INT(argv[0]) != 0) {
        JSValue next = JS_NewInt32(context, 0);
        if (JS_EnqueueJobOwned(context, NULL, NULL, staging_neutral_job, 1, &next) < 0)
            return JS_EXCEPTION;
    }
    return JS_UNDEFINED;
}

static JSValue staging_sibling_job(JSContext *context, int argc, JSValueConst *argv)
{
    (void)context; (void)argc; (void)argv;
    staging_sibling_jobs++;
    return JS_UNDEFINED;
}

static os64_js_status_t staging_install(os64_js_realm_t *realm, JSContext *context, void *opaque)
{
    unsigned mode = *(unsigned *)opaque;
    os64_js_outcome_t out;
    check(os64_js_realm_activate(realm, OS64_JS_ABI_ID, &out) == OS64_JS_BAD_ARGUMENT,
          "installation cannot activate before its guard completes");
    if (mode == 3) {
        staging_reports = (RealmReports){.context = context};
        realm_host(realm, &staging_reports);
    }
    const char *source = mode == 3 ? "Promise.reject(Error('private rejection'))" :
        "globalThis.installed=0;Promise.resolve().then(()=>installed++).then(()=>installed++)";
    JSValue value = JS_Eval(context, source, os64_strlen(source), "private-bootstrap", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(value)) return OS64_JS_EXCEPTION;
    JS_FreeValue(context, value);
    JSValue descend = JS_NewInt32(context, 1);
    if (JS_EnqueueJobOwned(context, NULL, NULL, staging_neutral_job, 1, &descend) < 0)
        return OS64_JS_EXCEPTION;
    if (mode == 4) {
        /* Two passes: cycle collection follows weak-reference processing. */
        JS_RunGC(JS_GetRuntime(context));
        JS_RunGC(JS_GetRuntime(context));
    }
    if (mode == 2) {
        JS_ThrowTypeError(context, "private bootstrap exception");
        return OS64_JS_EXCEPTION;
    }
    return mode == 1 || mode == 4 ? OS64_JS_ADMISSION_REFUSED : OS64_JS_OK;
}

static JSValue staging_nested(JSContext *context, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)context; (void)self; (void)argc; (void)argv;
    os64_js_outcome_t out, saved = *staging_outer;
    check(os64_js_realm_stage(staging_runtime, OS64_JS_ABI_ID, 256 * 1024, &staging_child, &out) == OS64_JS_OK,
          "nested staging reserves child before installation");
    if (!staging_child) return JS_UNDEFINED;
    check(os64_js_realm_setup(staging_child, OS64_JS_ABI_ID, staging_install, &staging_mode,
          staging_outer) == OS64_JS_BUSY && os64_memcmp(&saved, staging_outer, sizeof(saved)) == 0,
          "candidate setup protects parent outcome aliases");
    os64_js_status_t expected = staging_mode == 0 ? OS64_JS_OK : OS64_JS_ADMISSION_REFUSED;
    check(os64_js_realm_setup(staging_child, OS64_JS_ABI_ID, staging_install, &staging_mode, &out) == expected,
          "nested candidate installation has a local result");
    check(staging_sibling_jobs == 0 && os64_memcmp(&saved, staging_outer, sizeof(saved)) == 0,
          "candidate checkpoint leaves parent queue and active outcome alone");
    if (staging_mode == 0)
        check(os64_js_realm_activate(staging_child, OS64_JS_ABI_ID, &out) == OS64_JS_OK,
              "nested successful candidate activates without sibling checkpoint");
    else
        check(os64_js_realm_is_retired(staging_child) && staging_jobs == 0 && out.jobs_discarded == 2,
              "nested abort discards neutral and owned candidate work without running it");
    return JS_UNDEFINED;
}

static JSValue staging_at_cap(JSContext *context, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)context; (void)self; (void)argc; (void)argv;
    os64_js_outcome_t out;
    os64_js_realm_t *candidate = NULL;
    unsigned mode = 0;
    check(os64_js_realm_stage(staging_runtime, OS64_JS_ABI_ID, 256 * 1024, &candidate, &out) == OS64_JS_OK,
          "staging inside the parent's last allowed job succeeds without renewal");
    if (!candidate) return JS_UNDEFINED;
    JSContext *cc = os64_js_realm_context(candidate, OS64_JS_ABI_ID, &out);
    check(os64_js_realm_setup(candidate, OS64_JS_ABI_ID, staging_install, &mode, &out) == OS64_JS_LIMIT &&
          out.limit == OS64_JS_LIMIT_JOBS && out.jobs_executed == 1 && out.jobs_discarded == 2,
          "nested setup cannot renew the already-consumed parent job cap");
    JSValue value = realm_property(cc, "installed");
    check(JS_VALUE_GET_INT(value) == 0, "exhausted parent budget runs no candidate job");
    JS_FreeValue(cc, value);
    return JS_UNDEFINED;
}

static os64_js_status_t staging_native_install(os64_js_realm_t *realm, JSContext *context, void *opaque)
{
    (void)realm;
    JSValue global = JS_GetGlobalObject(context);
    JSValue function = JS_NewCFunctionDataNeutral(context, retirement_native_step, 1, 0, 0, NULL);
    if (JS_IsException(function)) { JS_FreeValue(context, global); return OS64_JS_EXCEPTION; }
    int result = JS_SetPropertyStr(context, global, "neutral", function);
    JS_FreeValue(context, global);
    if (result < 0) return OS64_JS_EXCEPTION;
    const char *source = "Promise.resolve().then(neutral)";
    JSValue value = JS_Eval(context, source, os64_strlen(source), "private-native-continuation", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(value)) return OS64_JS_EXCEPTION;
    JS_FreeValue(context, value);
    return *(bool *)opaque ? OS64_JS_ADMISSION_REFUSED : OS64_JS_OK;
}

static void staging_native_cases(unsigned policy)
{
    for (unsigned abort = 0; abort < 2; ++abort) {
        os64_js_realm_t *a, *b, *c, *candidate = NULL;
        os64_js_runtime_t *runtime = retirement_fixture(policy, 100, &a, &b, &c);
        if (!runtime) return;
        os64_js_outcome_t out;
        check(os64_js_realm_stage(runtime, OS64_JS_ABI_ID, 256 * 1024, &candidate, &out) == OS64_JS_OK,
              "native continuation candidate staged");
        retirement_native_hits = 0;
        bool refuse = abort != 0;
        check(os64_js_realm_setup(candidate, OS64_JS_ABI_ID, staging_native_install, &refuse, &out) ==
              (refuse ? OS64_JS_ADMISSION_REFUSED : OS64_JS_OK), "neutral candidate setup result");
        check(retirement_native_hits == (refuse ? 0u : 1u) &&
              out.jobs_discarded == (refuse ? 1u : 0u), "neutral ownership preserves candidate commit and abort tagging");
        check(realm_execute(a, "true", &out) == OS64_JS_OK && retirement_native_hits == (refuse ? 0u : 1u),
              "aborted neutral continuation cannot escape into sibling checkpoint");
        os64_js_destroy(runtime);
    }
}

static void staging_cases(void)
{
    for (unsigned policy = 0; policy < 2; policy++) {
        staging_native_cases(policy);
        for (unsigned mode = 0; mode < 5; mode++) {
            os64_js_realm_t *a, *b, *c;
            os64_js_runtime_t *runtime = retirement_fixture(policy, 100, &a, &b, &c);
            if (!runtime) continue;
            os64_js_outcome_t out;
            JSContext *ac = os64_js_realm_context(a, OS64_JS_ABI_ID, &out);
            JSContext *cc = os64_js_realm_context(c, OS64_JS_ABI_ID, &out);
            JS_SetGCThreshold(JS_GetRuntime(ac), (size_t)-1);
            if (mode == 4)
                check(realm_execute(c, "globalThis.cleaned=0;globalThis.reg=new FinalizationRegistry(()=>cleaned++);"
                      "(()=>{let x={};x.self=x;reg.register(x,1)})()", &out) == OS64_JS_OK,
                      "sibling registry awaits collection during child setup");
            os64_js_realm_t *candidate = NULL;
            check(os64_js_realm_stage(runtime, OS64_JS_ABI_ID, 256 * 1024, &candidate, &out) == OS64_JS_OK,
                  "private staged realm created");
            if (!candidate) { os64_js_destroy(runtime); continue; }
            check(realm_execute(candidate, "throw 1", &out) == OS64_JS_REALM_INSTALLING &&
                  os64_js_realm_task_begin(candidate, OS64_JS_ABI_ID, "closed-stage", &out) == OS64_JS_REALM_INSTALLING,
                  "ordinary run and task entry reject installing realm");
            check(os64_js_realm_activate(candidate, OS64_JS_ABI_ID, &out) == OS64_JS_BAD_ARGUMENT,
                  "staging cannot skip trusted setup");
            staging_jobs = 0;
            os64_js_status_t expected = mode == 0 ? OS64_JS_OK : mode == 2 ? OS64_JS_EXCEPTION :
                mode == 3 ? OS64_JS_UNHANDLED_REJECTION : OS64_JS_ADMISSION_REFUSED;
            check(os64_js_realm_setup(candidate, OS64_JS_ABI_ID, staging_install, &mode, &out) == expected,
                  "standalone candidate setup reports success or isolated failure");
            if (mode == 0) {
                check(staging_jobs == 2 && out.jobs_executed == 4 && !out.jobs_pending,
                      "candidate checkpoint drains descendants and neutral work under one budget");
                check(realm_execute(candidate, "true", &out) == OS64_JS_REALM_INSTALLING &&
                      os64_js_realm_activate(candidate, OS64_JS_ABI_ID, &out) == OS64_JS_OK &&
                      realm_execute(candidate, "if(installed!==2)throw installed", &out) == OS64_JS_OK,
                      "successful private setup requires explicit activation");
            } else {
                check(os64_js_realm_is_retired(candidate) &&
                      os64_js_realm_activate(candidate, OS64_JS_ABI_ID, &out) == OS64_JS_REALM_RETIRED,
                      "failed candidate remains retained and cannot activate");
                if (mode != 3) check(staging_jobs == 0, "failed setup never executes queued neutral work");
                if (mode == 3)
                    check(staging_reports.exceptions == 0 && staging_reports.rejected == 0,
                          "private rejection never calls staged script host");
            }
            if (mode == 4) {
                check(os64_js_drain_jobs(runtime, 20, &out) == OS64_JS_OK,
                      "sibling finalizer survives unrelated installation abort");
                JSValue global = JS_GetGlobalObject(cc);
                JSValue value = JS_GetPropertyStr(cc, global, "cleaned");
                check(JS_VALUE_GET_INT(value) == 1, "GC-triggered sibling cleanup was not tagged as candidate work");
                JS_FreeValue(cc, value); JS_FreeValue(cc, global);
            }
            check(realm_execute(a, "true", &out) == OS64_JS_OK && realm_execute(c, "true", &out) == OS64_JS_OK,
                  "candidate result leaves parent and sibling usable");
            os64_js_destroy(runtime);
        }
        for (unsigned mode = 0; mode < 2; mode++) {
            os64_js_realm_t *a, *b, *c;
            staging_runtime = retirement_fixture(policy, 100, &a, &b, &c);
            if (!staging_runtime) continue;
            os64_js_outcome_t out;
            JSContext *ac = os64_js_realm_context(a, OS64_JS_ABI_ID, &out);
            JSValue global = JS_GetGlobalObject(ac);
            JS_SetPropertyStr(ac, global, "stageNested", JS_NewCFunction(ac, staging_nested, "stageNested", 0));
            JS_FreeValue(ac, global);
            staging_outer = &out; staging_mode = mode; staging_jobs = 0; staging_sibling_jobs = 0; staging_child = NULL;
            check(realm_execute(a, "globalThis.doStage=()=>{Promise.resolve().then(()=>log.push('first'));"
                  "stageNested();Promise.resolve().then(()=>log.push('last'))}", &out) == OS64_JS_OK,
                  "parent callback compiled before task entry");
            JSValue callback = realm_property(ac, "doStage");
            check(os64_js_task_begin(staging_runtime, OS64_JS_ABI_ID, "parent-stage-task", &out) == OS64_JS_OK,
                  "parent task begins before nested stage");
            check(JS_EnqueueJob(ac, staging_sibling_job, 0, NULL) == 0,
                  "parent job queued before candidate transaction");
            check(os64_js_realm_call(a, OS64_JS_ABI_ID, callback, JS_UNDEFINED, 0, NULL, NULL, &out) == OS64_JS_OK && staging_sibling_jobs == 0,
                  "nested stage returns without running parent jobs");
            check(os64_js_task_end(staging_runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK && staging_sibling_jobs == 1,
                  "parent closes its own checkpoint after candidate result");
            check(realm_execute(a, "if(log.join(',')!=='first,last')throw log", &out) == OS64_JS_OK,
                  "candidate-only drain preserves parent FIFO");
            JS_FreeValue(ac, callback);
            os64_js_destroy(staging_runtime);
        }
        {
            os64_js_realm_t *a, *b, *c;
            staging_runtime = retirement_fixture(policy, 1, &a, &b, &c);
            if (staging_runtime) {
                os64_js_outcome_t out;
                JSContext *ac = os64_js_realm_context(a, OS64_JS_ABI_ID, &out);
                JSValue global = JS_GetGlobalObject(ac);
                JS_SetPropertyStr(ac, global, "stageAtCap", JS_NewCFunction(ac, staging_at_cap, "stageAtCap", 0));
                JS_FreeValue(ac, global);
                check(realm_execute(a, "Promise.resolve().then(stageAtCap)", &out) == OS64_JS_LIMIT &&
                      out.limit == OS64_JS_LIMIT_JOBS, "running parent job retains cap across candidate setup");
                os64_js_destroy(staging_runtime);
            }
        }
        os64_js_realm_t *a, *b, *c, *candidate = NULL;
        os64_js_runtime_t *runtime = retirement_fixture(policy, 1, &a, &b, &c);
        if (!runtime) continue;
        os64_js_outcome_t out;
        unsigned mode = 0;
        check(os64_js_realm_stage(runtime, OS64_JS_ABI_ID, 256 * 1024, &candidate, &out) == OS64_JS_OK,
              "budgeted candidate staged");
        check(os64_js_realm_setup(candidate, OS64_JS_ABI_ID, staging_install, &mode, &out) == OS64_JS_LIMIT &&
              out.limit == OS64_JS_LIMIT_JOBS && out.jobs_executed == 1 && out.jobs_discarded == 2 &&
              os64_js_realm_is_retired(candidate), "candidate jobs consume enclosing cap and abort remaining work");
        check(realm_execute(c, "true", &out) == OS64_JS_FAILED_RUNTIME,
              "candidate overrun retains whole-runtime failure");
        os64_js_destroy(runtime);
    }
}
#endif
