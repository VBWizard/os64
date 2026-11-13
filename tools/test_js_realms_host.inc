/* Host observations that need syscall, clock and allocation injection. */
static void realm_output_cases(void)
{
    os64_js_config_t config = fixture_config();
    os64_js_runtime_t *runtime = create_fixture(&config);
    if (!runtime) return;
    os64_js_outcome_t out;
    os64_js_realm_t *a = os64_js_primary_realm(runtime, OS64_JS_ABI_ID, &out), *b = NULL;
    check(os64_js_realm_create(runtime, OS64_JS_ABI_ID, &b, &out) == OS64_JS_OK, "console child created");
    if (!b) { os64_js_destroy(runtime); return; }
    JSContext *ac = os64_js_realm_context(a, OS64_JS_ABI_ID, &out);
    JSContext *bc = os64_js_realm_context(b, OS64_JS_ABI_ID, &out);
    check(os64_js_realm_install_output(a, 101, OS64_JS_OUTPUT_CONSOLE | OS64_JS_OUTPUT_PRINT, &out) == OS64_JS_OK &&
          os64_js_realm_install_output(b, 102, OS64_JS_OUTPUT_CONSOLE | OS64_JS_OUTPUT_PRINT, &out) == OS64_JS_OK,
          "two output handles installed independently");
    realm_output_recording = true;
    realm_output_lengths[0] = realm_output_lengths[1] = 0;
    check(realm_execute(a, "console.group('A');console.count('x');console.time('t')", &out) == OS64_JS_OK &&
          realm_execute(b, "console.count('x');console.time('t');globalThis.childLog=console.log;globalThis.childPrint=print", &out) == OS64_JS_OK,
          "realm console groups counters and timers initialized");
    realm_share(bc, ac, "childLog"); realm_share(bc, ac, "childPrint");
    check(realm_execute(a, "childLog('borrowed');childPrint('printed');console.count('x');console.timeEnd('t')", &out) == OS64_JS_OK &&
          realm_execute(b, "console.count('x');console.timeEnd('t')", &out) == OS64_JS_OK,
          "borrowed child console method invoked by parent");
    realm_output_recording = false;
    realm_output_bytes[0][realm_output_lengths[0]] = 0;
    realm_output_bytes[1][realm_output_lengths[1]] = 0;
    check(!strcmp(realm_output_bytes[0], "A\n  x: 1\n  x: 2\n  t: 0.000ms\n") &&
          !strcmp(realm_output_bytes[1], "x: 1\nborrowed\nprinted\nx: 2\nt: 0.000ms\n"),
          "console handle group depth counters and timers stay with installing realm");
    os64_js_destroy(runtime);
}

typedef struct { os64_js_realm_t *selected; unsigned calls; } RealmRouter;
static os64_js_realm_t *realm_route(void *opaque, JSContext *context, JSValue promise, bool handled)
{
    RealmRouter *route = opaque;
    (void)context; (void)promise; (void)handled;
    route->calls++;
    return route->selected;
}
static void realm_router_cases(void)
{
    os64_js_config_t config = fixture_config();
    os64_js_runtime_t *runtime = create_fixture(&config), *other = create_fixture(&config);
    if (!runtime || !other) { os64_js_destroy(runtime); os64_js_destroy(other); return; }
    os64_js_outcome_t out;
    os64_js_realm_t *a = os64_js_primary_realm(runtime, OS64_JS_ABI_ID, &out), *b = NULL;
    check(os64_js_realm_create(runtime, OS64_JS_ABI_ID, &b, &out) == OS64_JS_OK, "router child created");
    if (!b) { os64_js_destroy(runtime); os64_js_destroy(other); return; }
    JSContext *ac = os64_js_realm_context(a, OS64_JS_ABI_ID, &out);
    JSContext *bc = os64_js_realm_context(b, OS64_JS_ABI_ID, &out);
    /* Hooks retain the native callback context, even when policy selects a
     * different host. Here the parent host expects the child's context. */
    RealmReports ar = {.context = bc}, br = {.context = bc};
    realm_host(a, &ar); realm_host(b, &br);
    RealmRouter route = {.selected = a};
    os64_js_rejection_router_t router = {&route, realm_route};
    check(os64_js_set_rejection_router(runtime, OS64_JS_ABI_ID, &router, &out) == OS64_JS_OK,
          "native rejection router installed");
    check(realm_execute(b, "globalThis.p=Promise.reject(61)", &out) == OS64_JS_OK &&
          realm_take(a, ac, 61) && !realm_take(b, bc, 0) && ar.rejected == 1 && br.rejected == 0,
          "router selects parent report owner from child native context");
    route.selected = b;
    check(realm_execute(b, "p.catch(()=>{})", &out) == OS64_JS_OK && br.handled == 1,
          "handled-after-take uses router's current selection");
    route.selected = a;
    check(realm_execute(b, "globalThis.p=Promise.reject(62)", &out) == OS64_JS_OK, "routed pending rejection created");
    unsigned calls = route.calls;
    route.selected = os64_js_primary_realm(other, OS64_JS_ABI_ID, &out);
    realm_share(bc, ac, "p");
    check(realm_execute(a, "Promise.prototype.then.call(p,undefined,()=>{})", &out) == OS64_JS_OK &&
          route.calls == calls && !realm_take(a, ac, 0), "handled-before-take bypasses even a foreign router result");
    route.selected = NULL;
    check(realm_execute(b, "Promise.reject(63)", &out) == OS64_JS_OK && !realm_take(a, ac, 0) && !realm_take(b, bc, 0),
          "router may suppress a new rejection");
    route.selected = os64_js_primary_realm(other, OS64_JS_ABI_ID, &out);
    check(realm_execute(b, "Promise.reject(64)", &out) == OS64_JS_HOST_FAILURE &&
          realm_execute(a, "true", &out) == OS64_JS_FAILED_RUNTIME, "foreign router result fails safely runtime-wide");
    os64_js_destroy(runtime); os64_js_destroy(other);
}

static void realm_allocation_cases(void)
{
    /* Discover both the target arena and sanitized allocation inventories.
     * Refuse each allocation once, retaining leak checks on both engines. */
    for (unsigned operation = 0; operation < 3; operation++) {
        size_t count = 0;
        for (size_t cut = 0; cut <= count; cut++) {
            size_t before_live = live;
            os64_js_config_t config = fixture_config();
            os64_js_runtime_t *runtime = create_fixture(&config);
            if (!runtime) return;
            os64_js_outcome_t out;
            os64_js_realm_t *child = NULL;
            if (operation != 0 && os64_js_realm_create(runtime, OS64_JS_ABI_ID, &child, &out) != OS64_JS_OK) {
                check(false, "allocation sweep child setup"); os64_js_destroy(runtime); return;
            }
            size_t start = attempts;
            if (cut != 0) fail_at = start + cut;
            os64_js_status_t result = operation == 0
                ? os64_js_realm_create(runtime, OS64_JS_ABI_ID, &child, &out)
                : operation == 1
                    ? os64_js_realm_install_output(child, 101, OS64_JS_OUTPUT_CONSOLE | OS64_JS_OUTPUT_PRINT, &out)
                    : os64_js_realm_install_intl(child, "en-US", &out);
            fail_at = 0;
            if (cut == 0) {
                count = attempts - start;
                check(result == OS64_JS_OK && count != 0, "realm sweep discovers successful allocation inventory");
            } else {
                check(result == OS64_JS_HOST_FAILURE, "realm setup allocation refusal is sticky host failure");
                if (operation == 0) check(child == NULL, "failed realm construction publishes no handle");
                check(os64_js_run(runtime, "true", 4, "after failure", &out) == OS64_JS_FAILED_RUNTIME,
                      "child setup failure also refuses primary execution");
            }
            os64_js_destroy(runtime);
            check(live == before_live, "refused child construction or installer releases all allocations");
        }
        printf("realm allocation operation %u: %zu refusal points\n", operation, count);
    }
}

static void realm_deadline_cases(void)
{
    for (unsigned mode = 0; mode < 3; mode++) {
        os64_js_config_t config = fixture_config();
        config.limits.execution_ms = 10;
        os64_js_runtime_t *runtime = create_fixture(&config);
        if (!runtime) return;
        os64_js_outcome_t out;
        os64_js_realm_t *child = NULL;
        check(os64_js_task_begin(runtime, OS64_JS_ABI_ID, "parent-deadline", &out) == OS64_JS_OK,
              "deadline parent task begins");
        if (mode == 0) monotonic += 10000;
        else if (mode == 1) clock_error = -71;

        os64_js_status_t result = os64_js_realm_create(runtime, OS64_JS_ABI_ID, &child, &out);
        if (mode == 2 && result == OS64_JS_OK) {
            monotonic += 10000;
            result = os64_js_realm_install_intl(child, "en-US", &out);
        }
        clock_error = 0; clock_step = 0;
        check(result == (mode == 1 ? OS64_JS_HOST_FAILURE : OS64_JS_LIMIT),
              "child setup inherits parent deadline and clock failure");
        if (mode != 2) check(child == NULL, "expired or broken setup publishes no child");
        check(os64_js_task_end(runtime, OS64_JS_ABI_ID, &out) == result && !os64_js_turn_open(runtime),
              "setup failure task-end preserves sticky outcome and closes task");
        os64_js_destroy(runtime);
    }
}

static void realm_teardown_cases(void)
{
    for (unsigned policy = 0; policy < 2; policy++) for (unsigned repeat = 0; repeat < 3; repeat++) {
        size_t before = live;
        os64_js_config_t config = fixture_config();
        os64_js_runtime_t *runtime = NULL;
        os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config, policy ? OS64_JS_TEARDOWN_RECLAIM : OS64_JS_TEARDOWN_FATAL,
              OS64_JS_ABI_ID, &runtime, &out) == OS64_JS_OK, "teardown policy realm runtime created");
        if (!runtime) return;
        os64_js_realm_t *a = os64_js_primary_realm(runtime, OS64_JS_ABI_ID, &out), *b = NULL;
        check(os64_js_realm_create(runtime, OS64_JS_ABI_ID, &b, &out) == OS64_JS_OK, "teardown child created");
        if (!b) { os64_js_destroy(runtime); return; }
        JSContext *ac = os64_js_realm_context(a, OS64_JS_ABI_ID, &out);
        JSContext *bc = os64_js_realm_context(b, OS64_JS_ABI_ID, &out);
        RealmReports ar = {.context = ac}, br = {.context = bc};
        realm_host(a, &ar); realm_host(b, &br);
        check(realm_execute(a, "globalThis.parent=()=>42;Promise.reject(1)", &out) == OS64_JS_OK &&
              realm_execute(b, "globalThis.child=()=>43;Promise.reject(2)", &out) == OS64_JS_OK,
              "teardown retains both realm rejection records");
        realm_share(ac, bc, "parent"); realm_share(bc, ac, "child");
        os64_js_module_t *module = NULL;
        const char *source = "await new Promise(()=>{})";
        check(os64_js_realm_module_compile(b, OS64_JS_ABI_ID, source, strlen(source), "pending", NULL, &module, &out) == OS64_JS_OK &&
              os64_js_task_begin(runtime, OS64_JS_ABI_ID, "pending teardown", &out) == OS64_JS_OK &&
              os64_js_realm_module_run(b, OS64_JS_ABI_ID, module, &out) == OS64_JS_OK &&
              JS_EnqueueJob(bc, realm_throw_job, 0, NULL) == 0,
              "teardown retains pending module, shared functions and queued child job");
        os64_js_teardown_report_t report;
        os64_js_destroy_report(runtime, &report);
        check(!report.leaked && live == before, "whole-runtime teardown releases both realms cleanly under both policies");
    }
}

static void realm_watcher_cases(void)
{
    os64_js_config_t config = fixture_config();
    os64_js_runtime_t *runtime = create_fixture(&config);
    if (!runtime) return;
    os64_js_outcome_t out;
    os64_js_realm_t *child = NULL;
    check(os64_js_realm_create(runtime, OS64_JS_ABI_ID, &child, &out) == OS64_JS_OK, "watcher child created");
    if (!child) { os64_js_destroy(runtime); return; }
    JSContext *context = os64_js_realm_context(child, OS64_JS_ABI_ID, &out);
    JSValue constructor = realm_property(context, "Promise");
    JSValue prototype = JS_GetPropertyStr(context, constructor, "prototype");
    realm_watch_then = JS_GetPropertyStr(context, prototype, "then");
    JS_FreeValue(context, prototype); JS_FreeValue(context, constructor);
    const char *source = "await new Promise(()=>{})";
    os64_js_module_t *module = NULL;
    check(os64_js_realm_module_compile(child, OS64_JS_ABI_ID, source, strlen(source), "watcher", NULL, &module, &out) == OS64_JS_OK &&
          os64_js_task_begin(runtime, OS64_JS_ABI_ID, "watcher-parent-task", &out) == OS64_JS_OK,
          "child module prepared for watcher observation");
    realm_watch_calls = 0;
    realm_watch_context = context;
    os64_js_status_t result = os64_js_realm_module_run(child, OS64_JS_ABI_ID, module, &out);
    realm_watch_context = NULL;
    check(result == OS64_JS_OK && realm_watch_calls == 1, "pending child evaluation installs one native watcher");
    JS_FreeValue(context, realm_watch_then);
    realm_watch_then = JS_UNDEFINED;
    check(os64_js_task_end(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK, "watcher observation task closes");
    os64_js_destroy(runtime);
}

static void realm_admission_host_cases(void)
{
    os64_js_config_t config = fixture_config();
    os64_js_runtime_t *runtime = create_fixture(&config);
    if (!runtime) return;
    os64_js_outcome_t out;
    os64_js_realm_t *child = NULL;
    size_t start = attempts;
    fail_at = start + 1;
    check(os64_js_realm_preflight(runtime, OS64_JS_ABI_ID, SIZE_MAX, &out) == OS64_JS_ADMISSION_REFUSED &&
          os64_js_realm_create_with_headroom(runtime, OS64_JS_ABI_ID, SIZE_MAX, &child, &out) == OS64_JS_ADMISSION_REFUSED &&
          attempts == start && child == NULL, "preflight and refusal attempt no host allocation");
    fail_at = 0;
    check(os64_js_run(runtime, "true", 4, "after-local-refusal", &out) == OS64_JS_OK,
          "allocation-free refusal sets no allocator failure flag");
    JSContext *context = os64_js_context(runtime, OS64_JS_ABI_ID, &out);
    fail_at = attempts + 1;
    void *memory = js_malloc(context, 2 * 1024 * 1024);
    fail_at = 0;
    check(memory == NULL, "raw allocator refusal reached before wrapper observation");
    js_free(context, memory);
    check(os64_js_realm_preflight(runtime, OS64_JS_ABI_ID, SIZE_MAX, &out) == OS64_JS_FAILED_RUNTIME,
          "preflight preserves unobserved engine failure instead of local refusal");
    check(os64_js_run(runtime, "true", 4, "after-engine-refusal", &out) == OS64_JS_HOST_FAILURE,
          "original allocator failure still becomes sticky at its normal observation");
    os64_js_destroy(runtime);
    config.limits.execution_ms = 10;
    runtime = create_fixture(&config);
    if (!runtime) return;
    check(os64_js_task_begin(runtime, OS64_JS_ABI_ID, "admission-expired-parent", &out) == OS64_JS_OK,
          "headroom deadline task begins");
    monotonic += 10000;
    check(os64_js_realm_preflight(runtime, OS64_JS_ABI_ID, 256 * 1024, &out) == OS64_JS_OK,
          "headroom snapshot does not observe or renew task time");
    check(os64_js_realm_create_with_headroom(runtime, OS64_JS_ABI_ID, 256 * 1024, &child, &out) == OS64_JS_LIMIT &&
          out.limit == OS64_JS_LIMIT_EXECUTION && child == NULL,
          "admitted creation retains expired parent deadline");
    check(os64_js_task_end(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_LIMIT,
          "deadline failure remains sticky after admission");
    os64_js_destroy(runtime);
}

static unsigned retirement_unwind_hits;
static JSValue retirement_unwind_probe(JSContext *context, JSValueConst self,
                                        int argc, JSValueConst *argv)
{
    (void)context; (void)self; (void)argc; (void)argv;
    retirement_unwind_hits++;
    return JS_UNDEFINED;
}
static void retirement_interrupt_cases(void)
{
    const char *cases[] = {"spinB()", "catchB()", "Promise.resolve().then(spinB)", "+coerceB", "crossB()"};
    for (unsigned policy=0; policy<2; policy++) for(unsigned n=0;n<sizeof(cases)/sizeof(cases[0]);n++) {
        size_t before=live;
        os64_js_realm_t *a,*b,*c;
        os64_js_runtime_t *runtime=retirement_fixture(policy,100,&a,&b,&c);
        if(!runtime)return;
        os64_js_outcome_t out;
        JSContext *ac=os64_js_realm_context(a,OS64_JS_ABI_ID,&out),*bc=os64_js_realm_context(b,OS64_JS_ABI_ID,&out),*cc=os64_js_realm_context(c,OS64_JS_ABI_ID,&out);
        check(install_native(runtime,"unwindProbe",retirement_unwind_probe)==1,"interrupt observation hook installed");
        realm_share(ac,bc,"unwindProbe");
        check(realm_execute(c,"globalThis.spinC=()=>{while(true){}}",&out)==OS64_JS_OK,"nested interrupted sibling prepared");
        realm_share(cc,bc,"spinC");
        check(realm_execute(b,"globalThis.spinB=()=>{let a=[];while(true)a.push({a})};"
              "globalThis.catchB=()=>{try{while(true){}}catch(e){unwindProbe()}finally{unwindProbe()}};"
              "globalThis.coerceB={[Symbol.toPrimitive](){while(true){}}};globalThis.crossB=()=>spinC()",&out)==OS64_JS_OK,
              "interrupt allocation/catch/finally/coercion fixtures installed");
        const char *names[]={"spinB","catchB","coerceB","crossB"};
        for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++)realm_share(bc,ac,names[i]);
        check(os64_js_set_execution_ms(runtime,10,&out)==OS64_JS_OK,"interrupt fixture deadline configured");
        retirement_unwind_hits=0;clock_step=2500;
        os64_js_status_t result=realm_execute(a,cases[n],&out);
        clock_step=0;
        check(result==OS64_JS_LIMIT && out.limit==OS64_JS_LIMIT_EXECUTION,
              "cross-realm interruption retains runtime-wide execution outcome");
        check(retirement_unwind_hits==0,"uncatchable interruption runs neither catch nor finally author work");
        check(realm_execute(c,"true",&out)==OS64_JS_FAILED_RUNTIME &&
              os64_js_realm_retire(b,OS64_JS_ABI_ID,&out)==OS64_JS_OK &&
              realm_execute(a,"true",&out)==OS64_JS_FAILED_RUNTIME,
              "retirement after interruption cannot clear shared sticky failure");
        os64_js_destroy(runtime);
        check(live==before,"interrupted nested heap/native frames release under selected teardown policy");
    }
}

static unsigned staging_deadline_calls;
static os64_js_status_t staging_deadline_install(os64_js_realm_t *realm, JSContext *context, void *opaque)
{
    (void)realm; (void)context; (void)opaque;
    staging_deadline_calls++;
    return OS64_JS_OK;
}

static void staging_host_cases(void)
{
    for (unsigned policy = 0; policy < 2; policy++) {
        size_t cuts = 0;
        for (size_t cut = 0; cut <= cuts; cut++) {
            size_t before = live;
            os64_js_config_t config = fixture_config();
            os64_js_runtime_t *runtime = NULL;
            os64_js_realm_t *candidate = NULL;
            os64_js_outcome_t out;
            check(os64_js_create_with_teardown(&config, policy ? OS64_JS_TEARDOWN_RECLAIM : OS64_JS_TEARDOWN_FATAL,
                  OS64_JS_ABI_ID, &runtime, &out) == OS64_JS_OK, "setup sweep runtime created");
            if (!runtime) break;
            check(os64_js_realm_stage(runtime, OS64_JS_ABI_ID, 256 * 1024, &candidate, &out) == OS64_JS_OK,
                  "setup sweep private candidate created");
            if (!candidate) { os64_js_destroy(runtime); break; }
            size_t start = attempts;
            unsigned mode = 0;
            fail_at = cut ? start + cut : 0;
            os64_js_status_t result = os64_js_realm_setup(candidate, OS64_JS_ABI_ID, staging_install, &mode, &out);
            fail_at = 0;
            if (!cut) {
                cuts = attempts - start;
                printf("staged setup policy %u: %zu refusal points\n", policy, cuts);
                check(result == OS64_JS_OK && cuts > 0, "setup sweep measured successful installation");
            } else {
                check(result == OS64_JS_HOST_FAILURE && os64_js_realm_is_retired(candidate),
                      "engine allocation refusal retires candidate and preserves sticky shared failure");
                check(os64_js_run(runtime, "true", 4, "setup-sweep-parent", &out) == OS64_JS_FAILED_RUNTIME,
                      "candidate cleanup cannot clear allocation failure");
            }
            os64_js_teardown_report_t report;
            os64_js_destroy_report(runtime, &report);
            check(!report.leaked && live == before, "setup allocation cutoff releases native and engine holds");
        }
        os64_js_config_t config = fixture_config();
        config.limits.execution_ms = 10;
        os64_js_runtime_t *runtime = NULL;
        os64_js_realm_t *candidate = NULL;
        os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config, policy ? OS64_JS_TEARDOWN_RECLAIM : OS64_JS_TEARDOWN_FATAL,
              OS64_JS_ABI_ID, &runtime, &out) == OS64_JS_OK, "staged deadline runtime created");
        if (!runtime) continue;
        check(os64_js_realm_stage(runtime, OS64_JS_ABI_ID, 256 * 1024, &candidate, &out) == OS64_JS_OK &&
              os64_js_task_begin(runtime, OS64_JS_ABI_ID, "expired-stage-parent", &out) == OS64_JS_OK,
              "candidate prepared before enclosing deadline expires");
        monotonic += 10000;
        staging_deadline_calls = 0;
        check(os64_js_realm_setup(candidate, OS64_JS_ABI_ID, staging_deadline_install, NULL, &out) == OS64_JS_LIMIT &&
              out.limit == OS64_JS_LIMIT_EXECUTION && staging_deadline_calls == 0 && os64_js_realm_is_retired(candidate),
              "setup borrows expired parent deadline without calling installer");
        os64_js_destroy(runtime);
    }
}

#include "test_js_quiescence.inc"

static void realm_host_cases(void)
{
    quiescence_cases();
    staging_host_cases();
    retirement_interrupt_cases();
    realm_admission_host_cases();
    realm_watcher_cases();
    realm_output_cases();
    realm_router_cases();
    realm_allocation_cases();
    realm_deadline_cases();
    realm_teardown_cases();
}
