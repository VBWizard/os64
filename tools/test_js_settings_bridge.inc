/* Host clock control proves query purity and inheritance of the task deadline. */
static JSValue bridge_clock_body(JSContext *ctx, void *opaque)
{
    os64_js_runtime_t *runtime = opaque;
    os64_js_outcome_t out;
    int64_t before = monotonic;
    clock_step = 1000;
    check(os64_js_settings_realm(runtime, OS64_JS_ABI_ID, OS64_JS_SETTINGS_ENTRY, &out) != NULL &&
          monotonic == before, "settings query never samples the clock");
    check(os64_js_settings_realm(runtime, "wrong", OS64_JS_SETTINGS_ENTRY, &out) == NULL &&
          out.status == OS64_JS_ABI_MISMATCH && monotonic == before, "query ABI refusal leaves clock unchanged");
    check(os64_js_settings_realm(runtime, OS64_JS_ABI_ID, (os64_js_settings_role_t)99, &out) == NULL &&
          out.status == OS64_JS_BAD_ARGUMENT && monotonic == before, "unknown settings role refuses without guessing");
    clock_step = 0;
    monotonic += 1000;
    return JS_NewInt32(ctx, 77);
}

static JSValue bridge_raw_query(JSContext *ctx, void *opaque)
{
    (void)ctx;
    os64_js_runtime_t *runtime = opaque;
    os64_js_outcome_t out;
    check(os64_js_settings_realm(runtime, OS64_JS_ABI_ID, OS64_JS_SETTINGS_ENTRY, &out) == NULL &&
          out.status == OS64_JS_ADMISSION_REFUSED, "raw entry alone or foreign raw context cannot publish checked settings");
    return JS_UNDEFINED;
}

static JSValue bridge_foreign_raw(JSContext *ctx, void *opaque)
{
    JSContext *foreign = JS_NewContext(JS_GetRuntime(ctx));
    check(foreign != NULL, "unregistered same-engine context created");
    JSValue value = JS_UNDEFINED;
    if (foreign) {
        check(JS_RunWithSettings(foreign, foreign, bridge_raw_query, opaque, &value), "trusted raw foreign-context scope entered");
        JS_FreeContext(foreign);
    }
    return value;
}

static void settings_bridge_host_cases(void)
{
    for (unsigned policy = 0; policy < 2; policy++) {
        os64_js_config_t config = fixture_config();
        config.limits.execution_ms = 5;
        os64_js_runtime_t *runtime = NULL;
        os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config, policy ? OS64_JS_TEARDOWN_RECLAIM : OS64_JS_TEARDOWN_FATAL,
              OS64_JS_ABI_ID, &runtime, &out) == OS64_JS_OK, "settings clock runtime created");
        if (!runtime) continue;
        os64_js_realm_t *realm = os64_js_primary_realm(runtime, OS64_JS_ABI_ID, &out);
        JSContext *ctx = os64_js_realm_context(realm, OS64_JS_ABI_ID, &out);
        JSValue value = JS_UNDEFINED;
        check(JS_RunWithSettings(ctx, ctx, bridge_raw_query, runtime, &value), "raw scope does not imply checked wrapper admission");
        check(os64_js_task_begin(runtime, OS64_JS_ABI_ID, "settings-clock", &out) == OS64_JS_OK,
              "settings clock task opened");
        check(os64_js_realm_with_settings(realm, NULL, OS64_JS_ABI_ID, bridge_foreign_raw, runtime, &value, &out) == OS64_JS_OK,
              "checked scope refuses unregistered engine contexts and restores membership");
        monotonic += 4000;
        check(os64_js_realm_with_settings(realm, NULL, OS64_JS_ABI_ID, bridge_clock_body, runtime, &value, &out) == OS64_JS_LIMIT &&
              out.limit == OS64_JS_LIMIT_EXECUTION && JS_VALUE_GET_INT(value) == 77,
              "settings body uses remaining task deadline without renewing it");
        check(os64_js_task_end(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_LIMIT,
              "settings deadline failure survives task closure");
        os64_js_destroy(runtime);
    }
}
