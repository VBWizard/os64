#ifndef OS64_JS_SETTINGS_BRIDGE_CASES_H
#define OS64_JS_SETTINGS_BRIDGE_CASES_H

typedef struct {
    os64_js_runtime_t *runtime;
    os64_js_realm_t *realm[3], *staged;
    JSContext *ctx[3];
    os64_js_outcome_t *outer;
    unsigned bodies, depth;
} SettingsBridge;
static SettingsBridge *settings_bridge;

static int bridge_number(os64_js_realm_t *realm)
{
    for (unsigned i = 0; i < 3; i++)
        if (realm == settings_bridge->realm[i]) return (int)i + 1;
    return 0;
}

static JSValue bridge_probe(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self; (void)argc; (void)argv;
    os64_js_outcome_t out;
    int entry = bridge_number(os64_js_settings_realm(settings_bridge->runtime, OS64_JS_ABI_ID,
                             OS64_JS_SETTINGS_ENTRY, &out));
    int incumbent = bridge_number(os64_js_settings_realm(settings_bridge->runtime, OS64_JS_ABI_ID,
                                 OS64_JS_SETTINGS_INCUMBENT, &out));
    return JS_NewInt32(ctx, 10 * entry + incumbent);
}

static JSValue bridge_call(JSContext *ctx, void *opaque)
{
    return JS_Call(ctx, *(JSValue *)opaque, JS_UNDEFINED, 0, NULL);
}

static JSValue bridge_nested(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    if (!argc) return JS_UNDEFINED;
    os64_js_outcome_t out, saved = *settings_bridge->outer;
    JSValue value = JS_UNDEFINED, callback = JS_DupValue(ctx, argv[0]);
    check(os64_js_realm_with_settings(settings_bridge->realm[1], settings_bridge->realm[2],
          OS64_JS_ABI_ID, bridge_call, &callback, &value, settings_bridge->outer) == OS64_JS_BUSY &&
          os64_memcmp(&saved, settings_bridge->outer, sizeof(saved)) == 0,
          "nested scope refuses aliased outer outcome without clearing it");
    check(os64_js_settings_realm(settings_bridge->runtime, OS64_JS_ABI_ID,
          OS64_JS_SETTINGS_ENTRY, settings_bridge->outer) == NULL &&
          os64_memcmp(&saved, settings_bridge->outer, sizeof(saved)) == 0,
          "settings query protects outer outcome alias");
    os64_js_status_t result = os64_js_realm_with_settings(settings_bridge->realm[1], settings_bridge->realm[2],
                             OS64_JS_ABI_ID, bridge_call, &callback, &value, &out);
    check(result == OS64_JS_OK || result == OS64_JS_EXCEPTION, "nested callback scope runs under borrowed guard");
    check(os64_memcmp(&saved, settings_bridge->outer, sizeof(saved)) == 0, "nested scope preserves outer outcome");
    JS_FreeValue(ctx, callback);
    return value;
}

static JSValue bridge_eval(JSContext *ctx, void *opaque)
{
    const char *source = opaque;
    settings_bridge->bodies++;
    return JS_Eval(ctx, source, os64_strlen(source), "settings-bridge.js", JS_EVAL_TYPE_GLOBAL);
}

static JSValue bridge_native(JSContext *ctx, void *opaque)
{
    (void)opaque;
    settings_bridge->bodies++;
    os64_js_outcome_t out;
    check(os64_js_quiescent(settings_bridge->runtime, OS64_JS_ABI_ID, &out) == OS64_JS_BUSY,
          "native settings body remains non-quiescent without JS frames");
    check(realm_execute(settings_bridge->realm[0], "throw 1", &out) == OS64_JS_BUSY,
          "settings body cannot bypass wrapper top-level reentry guard");
    check(os64_js_task_end(settings_bridge->runtime, OS64_JS_ABI_ID, &out) == OS64_JS_BUSY,
          "settings body cannot end its borrowed task");
    check(os64_js_check_budget(settings_bridge->runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK,
          "native settings body uses existing bounded budget");
    return bridge_probe(ctx, JS_UNDEFINED, 0, NULL);
}

static JSValue bridge_depth(JSContext *ctx, void *opaque)
{
    os64_js_outcome_t *active = opaque;
    if (active != NULL) {
        JSValue untouched = JS_NewInt32(ctx, 123);
        check(os64_js_realm_with_settings(settings_bridge->realm[0], NULL, OS64_JS_ABI_ID,
              bridge_native, NULL, &untouched, active) == OS64_JS_BUSY && JS_VALUE_GET_INT(untouched) == 123,
              "nested outcome chain protects intermediate scope aliases");
        check(os64_js_settings_realm(settings_bridge->runtime, OS64_JS_ABI_ID,
              OS64_JS_SETTINGS_ENTRY, active) == NULL && active->status == OS64_JS_OK,
              "query refuses an intermediate active outcome without clearing it");
    }
    unsigned level = ++settings_bridge->depth;
    os64_js_outcome_t out;
    JSValue value = JS_NewInt32(ctx, 123);
    os64_js_status_t result = os64_js_realm_with_settings(settings_bridge->realm[level % 3],
        settings_bridge->realm[2], OS64_JS_ABI_ID, bridge_depth, &out, &value, &out);
    check(result == (level < JS_SETTINGS_SCOPE_LIMIT ? OS64_JS_OK : OS64_JS_ADMISSION_REFUSED),
          "checked scope preserves engine nesting bound");
    check(JS_VALUE_GET_INT(value) == 123, "scope overflow preserves value output");
    check(os64_js_settings_realm(settings_bridge->runtime, OS64_JS_ABI_ID,
          OS64_JS_SETTINGS_ENTRY, &out) == settings_bridge->realm[(level - 1) % 3],
          "checked nested entries restore selected realm");
    settings_bridge->depth--;
    return value;
}

static JSValue bridge_retire(JSContext *ctx, void *opaque)
{
    (void)opaque;
    os64_js_outcome_t out;
    check(os64_js_realm_retire(settings_bridge->realm[2], OS64_JS_ABI_ID, &out) == OS64_JS_OK,
          "callback incumbent can retire during a scope");
    check(os64_js_settings_realm(settings_bridge->runtime, OS64_JS_ABI_ID,
          OS64_JS_SETTINGS_INCUMBENT, &out) == NULL && out.status == OS64_JS_REALM_RETIRED,
          "settings query rechecks retirement instead of exposing stale authority");
    check(os64_js_settings_realm(settings_bridge->runtime, OS64_JS_ABI_ID,
          OS64_JS_SETTINGS_ENTRY, &out) == settings_bridge->realm[0], "retirement leaves healthy entry identifiable");
    return JS_NewInt32(ctx, 5);
}

static JSValue bridge_cancel(JSContext *ctx, void *opaque)
{
    (void)opaque;
    os64_js_outcome_t out;
    os64_js_cancel(settings_bridge->runtime);
    check(os64_js_settings_realm(settings_bridge->runtime, OS64_JS_ABI_ID,
          OS64_JS_SETTINGS_ENTRY, &out) == NULL && out.status == OS64_JS_FAILED_RUNTIME,
          "pending cancellation closes settings queries before failure observation");
    return JS_NewInt32(ctx, 55);
}

static os64_js_status_t bridge_private_setup(os64_js_realm_t *realm, JSContext *ctx, void *opaque)
{
    (void)realm; (void)opaque;
    os64_js_outcome_t out;
    JSValue value = JS_NewInt32(ctx, 123);
    check(os64_js_realm_with_settings(settings_bridge->realm[0], NULL, OS64_JS_ABI_ID,
          bridge_native, NULL, &value, &out) == OS64_JS_REALM_INSTALLING && JS_VALUE_GET_INT(value) == 123,
          "private setup cannot enter published settings through a healthy sibling");
    check(os64_js_settings_realm(settings_bridge->runtime, OS64_JS_ABI_ID,
          OS64_JS_SETTINGS_ENTRY, &out) == NULL && out.status == OS64_JS_REALM_INSTALLING,
          "private setup exposes no published settings query");
    return OS64_JS_ADMISSION_REFUSED;
}

static void settings_bridge_cases(void)
{
    for (unsigned policy = 0; policy < 2; policy++) {
        os64_js_config_t config = fixture_config();
        os64_js_runtime_t *runtime = NULL, *other = NULL;
        os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config, policy ? OS64_JS_TEARDOWN_RECLAIM : OS64_JS_TEARDOWN_FATAL,
              OS64_JS_ABI_ID, &runtime, &out) == OS64_JS_OK, "settings bridge runtime created");
        if (!runtime) continue;
        SettingsBridge fixture = {.runtime = runtime, .outer = &out}; settings_bridge = &fixture;
        fixture.realm[0] = os64_js_primary_realm(runtime, OS64_JS_ABI_ID, &out);
        check(os64_js_realm_create(runtime, OS64_JS_ABI_ID, &fixture.realm[1], &out) == OS64_JS_OK &&
              os64_js_realm_create(runtime, OS64_JS_ABI_ID, &fixture.realm[2], &out) == OS64_JS_OK &&
              os64_js_realm_stage(runtime, OS64_JS_ABI_ID, 256 * 1024, &fixture.staged, &out) == OS64_JS_OK,
              "settings bridge live and unpublished realms created");
        if (!fixture.realm[1] || !fixture.realm[2] || !fixture.staged) { os64_js_destroy(runtime); continue; }
        check(os64_js_create(&config, OS64_JS_ABI_ID, &other, &out) == OS64_JS_OK, "foreign wrapper runtime created");
        os64_js_realm_t *foreign = other ? os64_js_primary_realm(other, OS64_JS_ABI_ID, &out) : NULL;
        for (unsigned i = 0; i < 3; i++) {
            fixture.ctx[i] = os64_js_realm_context(fixture.realm[i], OS64_JS_ABI_ID, &out);
            JSValue global = JS_GetGlobalObject(fixture.ctx[i]);
            check(JS_SetPropertyStr(fixture.ctx[i], global, "settings", JS_NewCFunction(fixture.ctx[i], bridge_probe, "settings", 0)) == 1 &&
                  JS_SetPropertyStr(fixture.ctx[i], global, "hop", JS_NewCFunction(fixture.ctx[i], bridge_nested, "hop", 1)) == 1,
                  "checked settings probes installed");
            JS_FreeValue(fixture.ctx[i], global);
        }
        JSContext *a = fixture.ctx[0], *b = fixture.ctx[1];
        check(realm_execute(fixture.realm[1], "globalThis.foreign=()=>settings();globalThis.bound=settings.bind(null)", &out) == OS64_JS_OK,
              "foreign script and native-only callbacks created");
        realm_share(b, a, "foreign"); realm_share(b, a, "bound");
        JSValue value = JS_NewInt32(a, 123);
        check(os64_js_realm_with_settings(fixture.realm[0], NULL, OS64_JS_ABI_ID, bridge_native, NULL, &value, &out) == OS64_JS_BAD_ARGUMENT &&
              fixture.bodies == 0 && JS_VALUE_GET_INT(value) == 123, "settings scope cannot invent an idle execution budget");
        check(os64_js_settings_realm(runtime, OS64_JS_ABI_ID, OS64_JS_SETTINGS_ENTRY, &out) == NULL &&
              out.status == OS64_JS_ADMISSION_REFUSED, "unscoped query has no fallback entry");
        check(os64_js_realm_task_begin(fixture.realm[0], OS64_JS_ABI_ID, "settings-bridge", &out) == OS64_JS_OK,
              "settings bridge borrows an explicit task");
        check(os64_js_realm_with_settings(fixture.realm[0], NULL, "wrong", bridge_native, NULL, &value, &out) == OS64_JS_ABI_MISMATCH &&
              os64_js_realm_with_settings(NULL, NULL, OS64_JS_ABI_ID, bridge_native, NULL, &value, &out) == OS64_JS_BAD_ARGUMENT &&
              os64_js_realm_with_settings(fixture.realm[0], NULL, OS64_JS_ABI_ID, NULL, NULL, &value, &out) == OS64_JS_BAD_ARGUMENT &&
              os64_js_realm_with_settings(fixture.realm[0], NULL, OS64_JS_ABI_ID, bridge_native, NULL, NULL, &out) == OS64_JS_BAD_ARGUMENT,
              "invalid scope ABI and required arguments refuse entry");
        check(os64_js_realm_with_settings(fixture.staged, NULL, OS64_JS_ABI_ID, bridge_native, NULL, &value, &out) == OS64_JS_REALM_INSTALLING &&
              os64_js_realm_with_settings(fixture.realm[0], fixture.staged, OS64_JS_ABI_ID, bridge_native, NULL, &value, &out) == OS64_JS_REALM_INSTALLING &&
              (!foreign || os64_js_realm_with_settings(fixture.realm[0], foreign, OS64_JS_ABI_ID, bridge_native, NULL, &value, &out) == OS64_JS_ADMISSION_REFUSED) &&
              fixture.bodies == 0 && JS_VALUE_GET_INT(value) == 123, "unpublished and foreign realms never enter body or replace result");
        check(os64_js_realm_setup(fixture.staged, OS64_JS_ABI_ID, bridge_private_setup, NULL, &out) == OS64_JS_ADMISSION_REFUSED &&
              fixture.bodies == 0, "private setup refusal preserves task and admits no settings body");
        JS_Throw(a, JS_NewInt32(a, 41));
        size_t before = os64_js_heap_bytes(runtime);
        check(os64_js_realm_with_settings(fixture.realm[0], fixture.realm[2], OS64_JS_ABI_ID, bridge_native, NULL, &value, &out) == OS64_JS_OK &&
              JS_VALUE_GET_INT(value) == 13 && os64_js_heap_bytes(runtime) == before, "checked native scope allocates nothing and selects captured incumbent");
        JSValue exception = JS_GetException(a);
        check(JS_IsNumber(exception) && JS_VALUE_GET_INT(exception) == 41, "checked scopes and queries preserve pending exception");
        JS_FreeValue(a, exception);
        const char *source = "if(settings()!==11||foreign()!==12||hop(bound)!==23||hop(foreign)!==22||settings()!==11)throw 1;"
                             "try{hop(()=>{throw 42});throw 2}catch(e){if(e!==42)throw e}"
                             "globalThis.delivered=0;Promise.resolve().then(()=>delivered++)";
        check(os64_js_realm_with_settings(fixture.realm[0], fixture.realm[2], OS64_JS_ABI_ID, bridge_eval, (void *)source, &value, &out) == OS64_JS_OK,
              "checked entry/incumbent selection and abrupt nested return are independent of task owner");
        JS_FreeValue(a, value); value = JS_UNDEFINED;
        JSValue delivered = realm_property(a, "delivered");
        check(JS_IsNumber(delivered) && JS_VALUE_GET_INT(delivered) == 0 && out.jobs_pending && out.jobs_executed == 0,
              "settings scopes leave queued jobs for task checkpoint");
        JS_FreeValue(a, delivered);
        check(os64_js_realm_with_settings(fixture.realm[0], NULL, OS64_JS_ABI_ID, bridge_depth, NULL, &value, &out) == OS64_JS_OK,
              "checked nested limit unwinds without poisoning task");
        check(os64_js_realm_with_settings(fixture.realm[0], NULL, OS64_JS_ABI_ID, bridge_eval, "throw 42", &value, &out) == OS64_JS_EXCEPTION && JS_IsException(value),
              "checked abrupt body returns exception value without reporting or consuming it");
        exception = JS_GetException(a);
        check(JS_IsNumber(exception) && JS_VALUE_GET_INT(exception) == 42, "abrupt body preserves exception identity"); JS_FreeValue(a, exception);
        check(os64_js_realm_with_settings(fixture.realm[0], fixture.realm[2], OS64_JS_ABI_ID, bridge_retire, NULL, &value, &out) == OS64_JS_OK,
              "active scope unwinds after incumbent retirement");
        check(os64_js_realm_with_settings(fixture.realm[2], NULL, OS64_JS_ABI_ID, bridge_native, NULL, &value, &out) == OS64_JS_REALM_RETIRED &&
              os64_js_realm_with_settings(fixture.realm[0], fixture.realm[2], OS64_JS_ABI_ID, bridge_native, NULL, &value, &out) == OS64_JS_REALM_RETIRED,
              "retired entry and incumbent refuse new scopes");
        check(os64_js_task_end(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK, "task checkpoint remains usable after scope refusal and exception");
        delivered = realm_property(a, "delivered");
        check(JS_VALUE_GET_INT(delivered) == 1, "task checkpoint delivers queued job once"); JS_FreeValue(a, delivered);
        check(os64_js_quiescent(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK, "finished scope and task release quiescence guard");
        check(os64_js_task_begin(runtime, OS64_JS_ABI_ID, "settings-cancel", &out) == OS64_JS_OK, "next scope task begins normally");
        check(os64_js_realm_with_settings(fixture.realm[0], NULL, OS64_JS_ABI_ID, bridge_cancel, NULL, &value, &out) == OS64_JS_CANCELLED &&
              JS_VALUE_GET_INT(value) == 55, "post-body sticky failure preserves transferred result");
        check(os64_js_realm_with_settings(fixture.realm[0], NULL, OS64_JS_ABI_ID, bridge_native, NULL, &value, &out) == OS64_JS_FAILED_RUNTIME &&
              JS_VALUE_GET_INT(value) == 55, "failed runtime refuses settings without replacing result");
        check(os64_js_task_end(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_CANCELLED, "scope does not clear task cancellation");
        if (other) os64_js_destroy(other);
        os64_js_destroy(runtime); settings_bridge = NULL;
    }
}
#endif
