/* A captures; B creates a proxy; C supplies native code; D owns the queue.
 * Each family runs through both teardown policies with independent roles. */
static os64_js_runtime_t *scheduled_runtime;
static os64_js_realm_t *scheduled_realms[4];
static JSContext *scheduled_contexts[4];
static unsigned scheduled_calls, scheduled_family;
static bool scheduled_nested, scheduled_clock_retire;
static size_t scheduled_cut, scheduled_started;

static JSValue scheduled_arm(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)ctx; (void)self; (void)argc; (void)argv;
    scheduled_started = attempts;
    if (scheduled_cut) fail_at = attempts + scheduled_cut;
    return JS_UNDEFINED;
}


static uint64_t scheduled_now(void *opaque)
{
    (void)opaque;
    if (scheduled_clock_retire) {
        scheduled_clock_retire = false;
        os64_js_outcome_t out;
        os64_js_realm_retire(scheduled_realms[1], OS64_JS_ABI_ID, &out);
    }
    return p2b_ms;
}


static JSValue scheduled_probe(JSContext *ctx, JSValueConst self, int argc,
                                JSValueConst *argv, int magic, JSValue *data)
{
    (void)magic; (void)data;
    os64_js_outcome_t out;
    check(os64_js_settings_realm(scheduled_runtime, OS64_JS_ABI_ID, OS64_JS_SETTINGS_ENTRY, &out)
          == scheduled_realms[1], "scheduled settings: associated entry is B");
    check(os64_js_settings_realm(scheduled_runtime, OS64_JS_ABI_ID, OS64_JS_SETTINGS_INCUMBENT, &out)
          == scheduled_realms[0], "scheduled settings: captured incumbent is A");
    check(os64_js_settings_realm(scheduled_runtime, OS64_JS_ABI_ID, OS64_JS_SETTINGS_CURRENT, &out)
          == scheduled_realms[2], "scheduled settings: executable current is C");
    check(ctx == scheduled_contexts[1], "scheduled settings: native data incoming follows entry");
    check(JS_IsUndefined(self), "scheduled settings: WebIDL undefined receiver");
    check(argc == (scheduled_family == 3 ? 0 : 1), "scheduled settings: callback arity retained");
    if (scheduled_family == 0) check(JS_IsNumber(argv[0]), "scheduled settings: frame timestamp retained");
    if (scheduled_family == 1 || scheduled_family == 2) {
        JSValue timed = JS_GetPropertyStr(ctx, argv[0], "didTimeout");
        check(JS_ToBool(ctx, timed) == (scheduled_family == 2), "scheduled settings: idle deadline route retained");
        JS_FreeValue(ctx, timed);
    }
    scheduled_calls++;
    JSValue code_global = JS_GetGlobalObject(scheduled_contexts[2]);
    check(JS_SetPropertyStr(ctx, code_global, "calls", JS_NewUint32(ctx, scheduled_calls)) >= 0,
          "scheduled settings: native progress recorded");
    JS_FreeValue(ctx, code_global);
    if (scheduled_nested && scheduled_calls == 1) {
        /* Native-only reentry must keep the captured backup when it queues
         * another callback, despite C executing under B's entry. */
        JSValue d = JS_GetGlobalObject(scheduled_contexts[3]);
        JSValue b = JS_GetGlobalObject(scheduled_contexts[1]);
        JSValue queue = JS_GetPropertyStr(ctx, d, "queueMicrotask");
        JSValue function = JS_GetPropertyStr(ctx, b, "p");
        JSValue result = JS_Call(ctx, queue, d, 1, &function);
        JS_FreeValue(ctx, queue); JS_FreeValue(ctx, function);
        JS_FreeValue(ctx, d); JS_FreeValue(ctx, b);
        if (JS_IsException(result)) return result;
        JS_FreeValue(ctx, result);
    }
    return JS_UNDEFINED;
}

static JSValue scheduled_retire(JSContext *ctx, JSValueConst self, int argc,
                                JSValueConst *argv, int magic)
{
    (void)ctx; (void)self; (void)argc; (void)argv;
    os64_js_outcome_t out;
    os64_js_realm_retire(scheduled_realms[magic], OS64_JS_ABI_ID, &out);
    return JS_UNDEFINED;
}

static bool scheduled_drive(os64_dom_t *dom, unsigned family, os64_js_outcome_t *out)
{
    if (family == 0) return os64_dom_animation_fire(dom, os64_dom_animation_next(dom), out);
    if (family == 1) return os64_dom_idle_run(dom, out);
    if (family == 2) return os64_dom_timer_fire(dom, os64_dom_timer_next(dom), out);
    return false;
}

static void dom_scheduled_settings_cases(void)
{
    static const char *const requests[] = {
        "globalThis.handle=D.requestAnimationFrame(B.p);",
        "globalThis.handle=D.requestIdleCallback(B.p);",
        "globalThis.handle=D.requestIdleCallback(B.p,{timeout:1});",
        "D.queueMicrotask(B.p);"
    };
    size_t job_cuts[2] = {0};
    for (int policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; policy++)
    for (unsigned family = 0; family < 4; family++)
    for (size_t mode = 0; mode < 13 + job_cuts[policy]; mode++) {
        if ((mode >= 12 && family != 3) || (mode == 9 && family == 3) || (mode == 10 && family != 1 && family != 2) ||
            (mode == 11 && family != 0 && family != 2)) continue;
        scheduled_clock_retire = false;
        p2b_ms = 1000; p2b_us = 1000000;
        scheduled_calls = 0; scheduled_family = family; scheduled_nested = mode == 0 && family == 3;
        os64_js_config_t config = fixture_config(); config.limits.memory_bytes = 64u << 20;
        os64_js_runtime_t *runtime = NULL; os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config, policy, OS64_JS_ABI_ID, &runtime, &out) == OS64_JS_OK,
              "scheduled settings: runtime created");
        if (!runtime) continue;
        scheduled_runtime = runtime;
        os64_dom_cluster_limits_t limits = {64u<<20, 16u<<20, 8u<<20, 256u<<20, 128u<<20};
        os64_dom_cluster_t *cluster = os64_dom_cluster_create_with_scheduled_settings(runtime, &limits, &out);
        check(cluster != NULL, "scheduled settings: opt-in cluster created");
        if (!cluster) { os64_js_destroy(runtime); continue; }
        os64_dom_t *bindings[4] = {0};
        scheduled_realms[0] = os64_js_primary_realm(runtime, OS64_JS_ABI_ID, &out);
        bool ready = true;
        for (int i = 0; i < 4; i++) {
            if (i) check(os64_js_realm_create(runtime, OS64_JS_ABI_ID, &scheduled_realms[i], &out) == OS64_JS_OK,
                         "scheduled settings: child created");
            scheduled_contexts[i] = os64_js_realm_context(scheduled_realms[i], OS64_JS_ABI_ID, &out);
            DomFixture f = fixture_configured(0, 0, &config); os64_js_destroy(f.runtime);
            os64_dom_options_t options = p2b_options("https://scheduled.example/");
            options.now_ms = scheduled_now;
            os64_dom_admission_t admission = os64_dom_create_in_realm(cluster, scheduled_realms[i], f.document, f.state, &options, &out);
            bindings[i] = admission.binding; ready &= admission.binding != NULL;
            check(admission.binding && admission.consumed, "scheduled settings: binding installed");
            if (!admission.consumed) { os64_page_state_free(f.state); os64_html_document_free(f.document); }
        }
        if (ready) {
            for (int i = 0; i < 4; i++) {
                JSContext *ctx = scheduled_contexts[i]; JSValue global = JS_GetGlobalObject(ctx);
                for (int j = 0; j < 4; j++) {
                    char name[] = {(char)('A'+j),0}, retire[] = "retireA"; retire[6] += j;
                    check(JS_SetPropertyStr(ctx, global, name, JS_GetGlobalObject(scheduled_contexts[j])) >= 0,
                          "scheduled settings: trusted realm exchange");
                    check(JS_SetPropertyStr(ctx, global, retire, JS_NewCFunctionMagic(ctx, scheduled_retire,
                          retire, 0, JS_CFUNC_generic_magic, j)) >= 0, "scheduled settings: retirement seam installed");
                }
                JS_FreeValue(ctx, global);
                realm_script(scheduled_realms[i], STORAGE_ASSERT "globalThis.errors=[];"
                    "addEventListener('error',e=>{errors.push(e.error);e.preventDefault()});", "scheduled settings: bootstrap");
            }
            JSContext *c = scheduled_contexts[2]; JSValue global = JS_GetGlobalObject(c);
            check(JS_SetPropertyStr(c, global, "probe", JS_NewCFunctionData(c, scheduled_probe, 1, 0, 0, NULL)) >= 0,
                  "scheduled settings: native C callback installed");
            check(JS_SetPropertyStr(c, global, "arm", JS_NewCFunction(c, scheduled_arm, "arm", 0)) >= 0,
                  "scheduled settings: allocation seam installed");
            JS_FreeValue(c, global);
            realm_script(scheduled_realms[1], "globalThis.p=new Proxy(C.probe,{});Object.setPrototypeOf(p,A.Function.prototype);",
                         "scheduled settings: associated proxy identity survives prototype change");
            if (mode >= 12) {
                realm_script(scheduled_realms[0],"globalThis.enqueue=()=>{C.arm();D.queueMicrotask(B.p)};",
                             "scheduled settings: isolated enqueue prepared");
                JSValue a = JS_GetGlobalObject(scheduled_contexts[0]);
                JSValue enqueue = JS_GetPropertyStr(c,a,"enqueue");
                check(os64_js_realm_task_begin(scheduled_realms[0],OS64_JS_ABI_ID,"enqueue-cut",&out) == OS64_JS_OK,
                      "scheduled settings: allocation task opened");
                scheduled_cut = mode - 12;
                os64_js_realm_call(scheduled_realms[0],OS64_JS_ABI_ID,enqueue,JS_UNDEFINED,0,NULL,NULL,&out);
                size_t measured = attempts - scheduled_started;
                fail_at = 0;
                if (scheduled_cut) {
                    check(measured >= scheduled_cut && out.status != OS64_JS_OK,
                          "scheduled settings: enqueue engine allocation refusal reached");
                } else {
                    check(out.status == OS64_JS_OK && out.jobs_pending && measured < 64,
                          "scheduled settings: enqueue allocation measurement");
                    job_cuts[policy] = measured < 64 ? measured : 0;
                    fprintf(stderr,"scheduled microtask enqueue allocations: policy=%d cuts=%zu\n",policy,measured);
                }
                JS_FreeValue(c,enqueue); JS_FreeValue(c,a);
                os64_js_task_end(runtime,OS64_JS_ABI_ID,&out);
                check(scheduled_calls == (scheduled_cut ? 0u : 1u),
                      "scheduled settings: refused enqueue cannot execute callback");
            } else if (mode == 5) {
                /* No prepared host scope: refusal must precede timeout getters. */
                const char *source = family == 0 ? "requestAnimationFrame(C.probe)" : family == 3 ? "queueMicrotask(C.probe)"
                    : "requestIdleCallback(C.probe,{get timeout(){globalThis.readOption=true;return 1}})";
                char raw[512]; snprintf(raw,sizeof(raw),"globalThis.readOption=false;try{%s;throw Error('accepted')}catch(e){if(e.name!=='InvalidStateError')throw e}if(readOption)throw Error('late capture')",source);
                JSValue value = JS_Eval(c,raw,strlen(raw),"unprepared-scheduler",JS_EVAL_TYPE_GLOBAL);
                check(!JS_IsException(value), "scheduled settings: unprepared conversion refuses before options");
                if (JS_IsException(value)) JS_FreeValue(c,JS_GetException(c));
                JS_FreeValue(c,value);
                check(!scheduled_drive(bindings[3],family,&out), "scheduled settings: refusal publishes no work");
                char job[768]; snprintf(job,sizeof(job),"globalThis.refused=false;Promise.resolve().then(()=>{try{%s}catch(e){refused=e.name==='InvalidStateError'}});",requests[family]);
                realm_script(scheduled_realms[0],job,"scheduled settings: unprepared Promise registration");
                realm_script(scheduled_realms[0],"assert(refused);","scheduled settings: Promise jobs do not inherit host preparation");
            } else if (mode == 9) {
                char source[512]; snprintf(source,sizeof(source),"B.weak=new B.WeakRef(B.p);%s%sB.p=null;",requests[family],
                    family == 0 ? "D.cancelAnimationFrame(handle);" : "D.cancelIdleCallback(handle);");
                realm_script(scheduled_realms[0],source,"scheduled settings: cancel queued capture");
                JS_RunGC(JS_GetRuntime(c));
                realm_script(scheduled_realms[0],"assert(B.weak.deref()===undefined);","scheduled settings: cancellation releases callback root");
                check(!scheduled_drive(bindings[3],family,&out),"scheduled settings: cancelled work cannot fire");
            } else if (mode == 10 || mode == 11) {
                char source[512];
                snprintf(source,sizeof(source),"try{%s;assert(false)}catch(e){assert(e.name==='InvalidStateError')}",
                    mode == 10 ? "D.requestIdleCallback(B.p,{get timeout(){retireA();return 1}})" : requests[family]);
                scheduled_clock_retire = mode == 11;
                realm_script(scheduled_realms[0],source,"scheduled settings: conversion or host clock retires captured settings");
                check(!scheduled_clock_retire,"scheduled settings: retirement clock was reached");
                check(!scheduled_drive(bindings[3],family,&out),"scheduled settings: reentrant retirement publishes no work");
            } else if (mode == 6 && family != 3) {
                dom_fail_at = dom_attempts + 1;
                check(os64_js_realm_run(scheduled_realms[0],requests[family],strlen(requests[family]),"scheduled-cut",&out)
                      == OS64_JS_EXCEPTION, "scheduled settings: native allocation refusal");
                dom_fail_at = 0;
                os64_dom_script_ended(bindings[0],"scheduled-cut",&out);
                check(!scheduled_drive(bindings[3],family,&out), "scheduled settings: allocation refusal leaves no queue entry");
            } else {
                if (mode == 7)
                    realm_script(scheduled_realms[1], "globalThis.p=new Proxy(new C.Function('while(true){}'),{});",
                                 "scheduled settings: interruptible callback");
                if (mode == 8)
                    realm_script(scheduled_realms[1], "globalThis.pair=Proxy.revocable(C.probe,{});p=pair.proxy;",
                                 "scheduled settings: revocable callback");
                char source[512]; snprintf(source,sizeof(source),"%s%s",requests[family],
                    family == 3 && mode >= 1 && mode <= 4 ? (const char *[]) {"","retireA();","retireB();","retireC();","retireD();"}[mode]
                    : mode == 8 ? "B.pair.revoke();" : family == 3 && mode == 0 ? "Promise.resolve().then(()=>{if(C.calls!==1)throw Error('FIFO')});" : "");
                if (mode == 8) {
                    strcat(source,"B.p=new B.Proxy(C.probe,{});");
                    strcat(source,requests[family]);
                }
                if (family == 3 && mode == 7) dom_clock_step = 10000;
                os64_js_status_t status = os64_js_realm_run(scheduled_realms[0],source,strlen(source),"scheduled-register",&out);
                dom_clock_step = 0;
                check(status == (family == 3 && mode == 7 ? OS64_JS_LIMIT : family == 3 && mode == 8 ? OS64_JS_EXCEPTION : OS64_JS_OK),
                      "scheduled settings: registration and job outcome");
                if (family == 3 && mode == 8) {
                    os64_js_outcome_t jobs = out;
                    while (jobs.jobs_pending) {
                        os64_js_status_t drained = os64_js_drain_jobs(runtime, UINT64_MAX, &jobs);
                        check(drained == OS64_JS_OK || drained == OS64_JS_MORE_JOBS,
                              "scheduled settings: host continues checkpoint after a job error");
                        if (drained != OS64_JS_OK && drained != OS64_JS_MORE_JOBS) break;
                    }
                }
                os64_dom_script_ended(bindings[0],"scheduled-register",&out);
                if (family != 3) {
                    if (mode >= 1 && mode <= 4)
                        check(os64_js_realm_retire(scheduled_realms[mode-1],OS64_JS_ABI_ID,&out) == OS64_JS_OK,
                              "scheduled settings: independent role retired");
                    if (mode == 7) dom_clock_step = 10000;
                    scheduled_drive(bindings[3],family,&out);
                    dom_clock_step = 0;
                    if (mode == 7) check(out.status == OS64_JS_LIMIT, "scheduled settings: nested delivery preserves terminal interruption");
                    else if (mode != 4) check(out.status == (mode == 8 ? OS64_JS_EXCEPTION : OS64_JS_OK),
                                             "scheduled settings: host delivery outcome");
                }
                if (mode == 8 && (family == 1 || family == 2)) {
                    bool ran = scheduled_drive(bindings[3],family,&out);
                    if (!ran || out.status != OS64_JS_OK)
                        fprintf(stderr,"scheduled successor family=%u ran=%d calls=%u status=%d message=%s\n",family,ran,scheduled_calls,out.status,out.message);
                    check(ran && out.status == OS64_JS_OK,
                          "scheduled settings: healthy successor runs after callback error");
                }
                if (mode == 8)
                    realm_script(scheduled_realms[2], "assert(errors.length===1&&errors[0] instanceof B.TypeError);",
                                 "scheduled settings: revoked error uses associated entry and existing report owner");
                check(scheduled_calls == ((mode == 0 || mode == 6 || mode == 8) ? (scheduled_nested ? 2u : 1u) : 0u),
                      "scheduled settings: independent retirement suppresses callback");
            }
            check(JS_GetEntryContext(JS_GetRuntime(c)) == NULL && JS_GetIncumbentContext(JS_GetRuntime(c)) == NULL,
                  "scheduled settings: scope restored at host return");
            for (int i=0;i<4;i++) memory_balanced(bindings[i],"scheduled settings before drain");
        }
        os64_dom_cluster_drain(cluster); os64_js_destroy(runtime); os64_dom_cluster_free(cluster);
        check(live==0&&dom_live==0&&!dom_allocations,"scheduled settings: roots and native records released");
    }
}
