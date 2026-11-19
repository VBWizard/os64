/* THE LEDGER through the runtime and the engine (os64_js_ledger_*,
 * js_engine.h; the arithmetic alone is tools/test_js_ledger_host.c): off it
 * answers nothing and costs the runtime nothing to ask; on, a script's
 * compile is COMPILE, its host calls are NATIVE by callee, a layout pushed
 * inside a host call is charged to that callee, script a host call runs is
 * ENGINE again, the kinds add up inside the host's own frame, and the
 * profile samples the running function by name; not started from inside a
 * call; reset and disable; and a
 * runtime destroyed with the ledger on and a frame left open. */

static os64_js_runtime_t *ledger_fixture;

static void ledger_spin(unsigned n)
{
    volatile unsigned sink = 0;
    for (unsigned i = 0; i < n; i++)
        sink += i;
    (void)sink;
}

/* magic 1 works, 2 forces a "layout" (a host LAYOUT frame), 3 calls its
 * argument back, 4 tries to start the ledger from inside the call. */
static os64_js_status_t ledger_inner_enable = OS64_JS_OK;
static JSValue ledger_native(JSContext *context, JSValueConst self, int argc, JSValueConst *argv,
                             int magic, JSValue *data)
{
    (void)self; (void)data;
    ledger_spin(20000);
    if (magic == 2) {
        os64_js_ledger_push(ledger_fixture, OS64_JS_LEDGER_LAYOUT);
        ledger_spin(50000);
        os64_js_ledger_pop(ledger_fixture);
    }
    if (magic == 4)
        ledger_inner_enable = os64_js_ledger_enable(ledger_fixture, 0, 0);
    if (magic == 3 && argc > 0) {
        JSValue r = JS_Call(context, argv[0], JS_UNDEFINED, 0, NULL);
        if (JS_IsException(r)) return r;
        JS_FreeValue(context, r);
    }
    return JS_UNDEFINED;
}

static int ledger_install(os64_js_runtime_t *runtime, const char *name, int magic)
{
    os64_js_outcome_t outcome;
    JSContext *context = os64_js_context(runtime, OS64_JS_ABI_ID, &outcome);
    if (context == NULL) return -1;
    JSValue global = JS_GetGlobalObject(context);
    JSValue value = JS_NewCFunctionData(context, ledger_native, 1, magic, 0, NULL);
    int result = JS_IsException(value) ? -1 : JS_SetPropertyStr(context, global, name, value);
    JS_FreeValue(context, global);
    return result;
}

static uint64_t ledger_sum(const os64_js_ledger_totals_t *t)
{
    uint64_t sum = 0;
    for (int k = 0; k < OS64_JS_LEDGER_KINDS; k++)
        sum += t->self[k];
    return sum;
}

static void ledger_cases(void)
{
    os64_js_config_t config = fixture_config();
    config.limits.execution_ms = 60000;
    os64_js_runtime_t *runtime = NULL;
    os64_js_outcome_t outcome;
    if (os64_js_create(&config, OS64_JS_ABI_ID, &runtime, &outcome) != OS64_JS_OK || runtime == NULL) {
        check(0, "ledger: the fixture runtime is made");
        return;
    }
    ledger_fixture = runtime;
    os64_js_ledger_totals_t t;
    check(ledger_install(runtime, "work", 1) == 1 && ledger_install(runtime, "layout", 2) == 1 &&
          ledger_install(runtime, "callback", 3) == 1 && ledger_install(runtime, "starter", 4) == 1,
          "ledger: host functions installed");
    check(execute_fixture(runtime, "starter();1", &outcome) == OS64_JS_OK && ledger_inner_enable == OS64_JS_BUSY &&
          !os64_js_ledger_totals(runtime, &t), "ledger: not started from inside a call");

    size_t cursor = 0;
    os64_js_ledger_callee_t c;
    os64_js_ledger_push(runtime, OS64_JS_LEDGER_ENGINE);
    os64_js_ledger_pop(runtime);
    check(!os64_js_ledger_totals(runtime, &t) && t.samples == 0 && !os64_js_ledger_callee(runtime, &cursor, &c),
          "ledger: off, it answers nothing and a push is nothing");
    check(os64_js_ledger_enable(runtime, OS64_JS_LEDGER_PROFILE, 0) == OS64_JS_BAD_ARGUMENT &&
          os64_js_ledger_enable(runtime, 0x80, 1) == OS64_JS_BAD_ARGUMENT &&
          os64_js_ledger_enable(NULL, 0, 0) == OS64_JS_BAD_ARGUMENT, "ledger: a bad period, flag or runtime refused");
    check(os64_js_ledger_enable(runtime, OS64_JS_LEDGER_PROFILE, 20000) == OS64_JS_OK, "ledger: enabled with a profile");

    static const char source[] =
        "function hot(){let s=0;for(let i=0;i<300000;i++)s=(s+i*7)%1000003;return s}"
        "for(let k=0;k<10;k++)hot();work();layout();callback(function(){hot()});";
    uint64_t before = os64_js_ledger_now();
    os64_js_ledger_push(runtime, OS64_JS_LEDGER_ENGINE);
    os64_js_status_t ran = os64_js_run(runtime, source, sizeof(source) - 1, "ledger.js", &outcome);
    os64_js_ledger_pop(runtime);
    uint64_t after = os64_js_ledger_now();
    check(ran == OS64_JS_OK, "ledger: the measured script runs");
    check(os64_js_ledger_totals(runtime, &t) && t.open == 0, "ledger: every frame closed");
    check(t.count[OS64_JS_LEDGER_COMPILE] >= 1 && t.self[OS64_JS_LEDGER_COMPILE] > 0, "ledger: the compile is COMPILE");
    check(t.count[OS64_JS_LEDGER_NATIVE] == 3, "ledger: three host calls");
    check(t.count[OS64_JS_LEDGER_LAYOUT] == 1 && t.self[OS64_JS_LEDGER_LAYOUT] > 0, "ledger: the host's layout frame");
    check(t.count[OS64_JS_LEDGER_ENGINE] >= 3, "ledger: script, the run's own door and script a host call ran are ENGINE");
    check(t.self[OS64_JS_LEDGER_ENGINE] > t.self[OS64_JS_LEDGER_NATIVE], "ledger: the hot loop is the interpreter's");
    check(ledger_sum(&t) > 0 && ledger_sum(&t) <= after - before, "ledger: the kinds add up within the host's frame");
    bool forced = false, rows = 0;
    unsigned seen = 0;
    cursor = 0;
    while (os64_js_ledger_callee(runtime, &cursor, &c)) {
        seen++;
        rows = rows || (c.function == (const void *)ledger_native);
        if (c.magic == 2) forced = c.forced == 1 && c.forced_ticks > 0 && c.calls == 1;
    }
    check(seen == 3 && rows, "ledger: one row per callee, keyed by the function it was made with and its magic");
    check(forced, "ledger: the layout is charged to the callee that forced it");
    bool hot = false;
    os64_js_ledger_sample_t s;
    cursor = 0;
    while (os64_js_ledger_sample(runtime, &cursor, &s))
        hot = hot || (!strcmp(s.at.name, "hot") && !strcmp(s.at.file, "ledger.js") && s.at.line == 1 && s.ticks > 0);
    check(t.samples > 0 && hot, "ledger: the profile samples the hot function by name, file and line");

    os64_js_ledger_reset(runtime);
    check(os64_js_ledger_totals(runtime, &t) && ledger_sum(&t) == 0 && t.samples == 0, "ledger: reset zeroes it");
    os64_js_ledger_disable(runtime);
    check(!os64_js_ledger_totals(runtime, &t), "ledger: disabled, it answers nothing");
    check(execute_fixture(runtime, "work();1", &outcome) == OS64_JS_OK, "ledger: disabled, script and host calls run");

    check(os64_js_ledger_enable(runtime, 0, 0) == OS64_JS_OK, "ledger: enabled again without a profile");
    os64_js_ledger_push(runtime, OS64_JS_LEDGER_ENGINE);
    os64_js_destroy(runtime);
    check(1, "ledger: a runtime destroyed with the ledger on and a frame open");
    ledger_fixture = NULL;
}
