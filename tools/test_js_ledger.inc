/* THE LEDGER through the runtime and the engine (os64_js_ledger_*,
 * js_engine.h; the arithmetic alone is tools/test_js_ledger_host.c): off it
 * answers nothing and costs the runtime nothing to ask; on, a script's
 * compile is COMPILE, its host calls are NATIVE by callee, a layout pushed
 * inside a host call is charged to that callee, script a host call runs is
 * ENGINE again, the kinds add up inside the host's own frame, and the
 * profile samples the running function by name; not started from inside a
 * call; reset and disable; and a
 * runtime destroyed with the ledger on and a frame left open. The collector's
 * counts, kept with or without the ledger, are collector_cases. */

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

/* THE COLLECTOR'S COUNTS (os64_js_collector, patch 0047): kept whether or not
 * the ledger runs. Cycles made by script and left behind are what an
 * allocation-asked run frees; every run is counted, walked and timed; the
 * bytes after a run that freed cycles are fewer than before; and the counts
 * agree with the ledger's collector frames run for run. */
static void collector_cases(void)
{
    os64_js_collector_t c;
    os64_js_collector(NULL, &c);
    check(c.runs == 0 && c.walked == 0 && c.ticks == 0, "collector: NULL reads zero");
    os64_js_config_t config = fixture_config();
    config.limits.execution_ms = 60000;
    os64_js_runtime_t *runtime = NULL;
    os64_js_outcome_t outcome;
    if (os64_js_create(&config, OS64_JS_ABI_ID, &runtime, &outcome) != OS64_JS_OK || runtime == NULL) {
        check(0, "collector: the fixture runtime is made");
        return;
    }
    os64_js_collector_t before;
    os64_js_collector(runtime, &before);
    check(before.threshold > 0, "collector: the next run's threshold is said");
    static const char cycles[] =
        "for(let i=0;i<200000;i++){const a={},b={a};a.b=b}1";
    check(execute_fixture(runtime, cycles, &outcome) == OS64_JS_OK, "collector: the cycles are made");
    os64_js_collector(runtime, &c);
    uint64_t runs = c.runs - before.runs;
    check(runs >= 1 && c.runs_by_allocation - before.runs_by_allocation == runs,
          "collector: the allocation threshold asked for every run");
    check(c.walked > before.walked && c.walked_most > 0 && c.walked_most <= c.walked,
          "collector: the runs walked the GC objects, and the most one run walked is said");
    check(c.freed - before.freed >= 1000, "collector: the runs freed the cycles left behind");
    check(c.bytes_after - before.bytes_after < c.bytes_before - before.bytes_before,
          "collector: a run that freed cycles leaves fewer bytes than it found");
    check(c.ticks > before.ticks && c.ticks_most > 0 && c.ticks_most <= c.ticks,
          "collector: each run timed on the ledger's clock, with the longest");
    check(os64_js_ledger_enable(runtime, 0, 0) == OS64_JS_OK, "collector: the ledger started beside it");
    os64_js_collector(runtime, &before);
    check(execute_fixture(runtime, cycles, &outcome) == OS64_JS_OK, "collector: the cycles are made again");
    os64_js_collector(runtime, &c);
    os64_js_ledger_totals_t t;
    check(os64_js_ledger_totals(runtime, &t) && c.runs - before.runs == t.count[OS64_JS_LEDGER_COLLECTOR] &&
          c.runs > before.runs, "collector: its runs are the ledger's collector frames, one for one");
    os64_js_destroy(runtime);
}

/* THE COLLECTOR UNDER THE LIMIT (patch 0048). With the heap's live part at
 * seven tenths of the limit, half as much again is past it: upstream's
 * threshold would wait for a heap the limit refuses, and a page making
 * garbage there would fail with the collector never asked. Here:
 * - garbage made with the live part at seven tenths, then at most a 128th
 *   of the limit from it, is collected and nothing is refused, and so is
 *   garbage of strings too large for the last stretch, which the margin
 *   leaves room for;
 * - a heap that is all live, from inside the margin (a sixteenth of the
 *   limit) to its refusal, is walked by at most 8 runs.
 * Each phase is a runtime of its own at the fixture's 16 MiB. */
#include "os64/fmt.h"
static os64_js_runtime_t *limit_runtime(void)
{
    os64_js_config_t config = fixture_config();
    config.limits.execution_ms = 60000;
    os64_js_runtime_t *runtime = NULL;
    os64_js_outcome_t outcome;
    if (os64_js_create(&config, OS64_JS_ABI_ID, &runtime, &outcome) != OS64_JS_OK) return NULL;
    return runtime;
}

/* Live objects, each with a string of about a kilobyte, until the heap holds
 * `share` thousandths of the engine's limit: in rounds that each go half the
 * way left at no less than 4 KiB an object, so none overshoots. */
static bool limit_fill(os64_js_runtime_t *runtime, uint64_t limit, unsigned share)
{
    char source[256];
    size_t target = (size_t)(limit * share / 1000);
    for (size_t heap = os64_js_heap_bytes(runtime); heap < target; heap = os64_js_heap_bytes(runtime)) {
        size_t count = (target - heap) / 2 / 4096 + 1;
        os64_snprintf(source, sizeof(source),
                      "globalThis.live=globalThis.live||[];for(let i=0;i<%lu;i++)live.push({s:'x'.repeat(1000)+i});1",
                      (unsigned long)count);
        os64_js_outcome_t outcome;
        if (execute_fixture(runtime, source, &outcome) != OS64_JS_OK) return false;
    }
    return true;
}

static const char kLimitGarbage[] =
    "for(let i=0;i<40000;i++){const a={s:'y'.repeat(1000)+i},b={a};a.b=b}1";
/* Each cycle's string is made after its objects, and is larger than the
 * last stretch (a 256th of the limit) but well inside the margin: a run that
 * waited for the last stretch would leave no room for it. */
static const char kLimitLargeGarbage[] =
    "for(let i=0;i<400;i++){const a={},b={a};a.b=b;a.s='w'.repeat(200000)+i}1";

static void collector_limit_cases(void)
{
    os64_js_outcome_t outcome;
    os64_js_runtime_t *runtime = limit_runtime();
    if (runtime == NULL) {
        check(0, "limit: the fixture runtime is made");
        return;
    }
    os64_js_collector_t c;
    os64_js_collector(runtime, &c);
    uint64_t limit = c.limit;
    check(limit > 0 && limit <= fixture_config().limits.memory_bytes, "limit: the engine's limit is said");
    check(limit_fill(runtime, limit, 700), "limit: the live part filled to seven tenths");
    check(execute_fixture(runtime, kLimitGarbage, &outcome) == OS64_JS_OK,
          "limit: garbage beside a live part of seven tenths is collected, nothing refused");
    os64_js_collector(runtime, &c);
    check(c.runs_by_limit >= 1 && c.refused_by_limit == 0,
          "limit: the runs there waited for a threshold the limit lowered");
    check(c.threshold < limit, "limit: the next threshold is below the limit");
    check(execute_fixture(runtime, kLimitLargeGarbage, &outcome) == OS64_JS_OK,
          "limit: garbage of strings larger than the last stretch is collected with room left for each");
    os64_js_collector(runtime, &c);
    check(c.refused_by_limit == 0, "limit: nothing refused for them");
    os64_js_destroy(runtime);

    runtime = limit_runtime();
    if (runtime == NULL) {
        check(0, "limit: the second runtime is made");
        return;
    }
    check(limit_fill(runtime, limit, 992), "limit: the live part filled to within a 128th of the limit");
    check(execute_fixture(runtime, kLimitGarbage, &outcome) == OS64_JS_OK,
          "limit: garbage in the last stretch before the limit is collected");
    os64_js_collector(runtime, &c);
    check(c.refused_by_limit == 0 && c.threshold < limit, "limit: nothing refused, and the threshold below the limit");
    os64_js_destroy(runtime);

    runtime = limit_runtime();
    if (runtime == NULL) {
        check(0, "limit: the third runtime is made");
        return;
    }
    check(limit_fill(runtime, limit, 950), "limit: the live part filled inside the margin");
    os64_js_collector_t before;
    os64_js_collector(runtime, &before);
    check(execute_fixture(runtime, "for(;;)live.push({s:'z'.repeat(1000)+live.length})", &outcome) != OS64_JS_OK,
          "limit: a heap that is all live is refused at last");
    os64_js_collector(runtime, &c);
    check(c.refused_by_limit > before.refused_by_limit, "limit: the refusal is counted");
    check(c.runs - before.runs <= 8, "limit: at most 8 runs walk an all-live heap from inside the margin to its refusal");
    os64_js_destroy(runtime);
}
