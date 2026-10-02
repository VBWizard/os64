// test_js_cli_fake.c — a stand-in for the os64_js_* runtime, so the js
// runner's own decisions can be tested on every outcome, including the ones
// the real library cannot be made to produce on demand (an ABI mismatch, a
// BUSY, a refused installer). test_js_cli_real.c runs it on the real library.
//
// It implements CONTRACT.md's calling rules and nothing else: every operation
// initializes the caller's outcome and returns the same status it stores
// there, create clears *out before anything can fail, installers copy what
// they are given and run once each before evaluation, destroy(NULL) is a
// no-op. What a call ANSWERS is scripted by the test; what it was ASKED is
// recorded, so a case can check the runner's whole call sequence. It does
// not evaluate JavaScript and has no job queue: a fake that grew either would
// be a second engine, which is exactly what the runner must not depend on.
#include <stdlib.h>
#include <string.h>
#include "os64/js.h"
#include "test_js_cli_fake.h"

js_fake_t js_fake;

struct os64_js_runtime { int alive; };
static struct os64_js_runtime s_runtime;

static void begin(os64_js_outcome_t *o, os64_js_status_t status)
{
    memset(o, 0, sizeof(*o));
    o->status = status;
}

static void record(const char *call)
{
    size_t n = strlen(js_fake.calls);
    if (n + strlen(call) + 2 < sizeof(js_fake.calls)) {
        if (n) strcat(js_fake.calls, " ");
        strcat(js_fake.calls, call);
    }
}

// A scripted answer, with the diagnostic fields a real failure would carry.
static os64_js_status_t answer(const js_fake_answer_t *a, os64_js_outcome_t *o)
{
    begin(o, a->status);
    o->limit = a->limit;
    o->host_error = a->host_error;
    o->line = a->line;
    o->column = a->column;
    o->diagnostic_truncated = a->truncated;
    strncpy(o->message, a->message, sizeof(o->message) - 1);
    strncpy(o->stack_trace, a->stack_trace, sizeof(o->stack_trace) - 1);
    if (a->source_name[0])
        strncpy(o->source_name, a->source_name, sizeof(o->source_name) - 1);
    return o->status;
}

static int bad_runtime(os64_js_runtime_t *rt)
{
    return rt != &s_runtime || !s_runtime.alive;
}

os64_js_status_t os64_js_create(const os64_js_config_t *config, const char *caller_abi,
                                os64_js_runtime_t **out, os64_js_outcome_t *outcome)
{
    record("create");
    *out = NULL;
    if (config == NULL || caller_abi == NULL)
        return answer(&(js_fake_answer_t){.status = OS64_JS_BAD_ARGUMENT}, outcome);
    js_fake.config = *config;
    js_fake.abi_matched = strcmp(caller_abi, OS64_JS_ABI_ID) == 0;
    const os64_js_limits_t *l = &config->limits;
    if (!l->memory_bytes || !l->stack_bytes || !l->source_bytes || !l->execution_ms ||
        !l->jobs_per_turn)
        return answer(&(js_fake_answer_t){.status = OS64_JS_BAD_ARGUMENT}, outcome);
    if (js_fake.create.status != OS64_JS_OK)
        return answer(&js_fake.create, outcome);
    s_runtime.alive = 1;
    js_fake.installed_output = js_fake.installed_args = js_fake.evaluated = 0;
    *out = &s_runtime;
    begin(outcome, OS64_JS_OK);
    return OS64_JS_OK;
}

os64_js_status_t os64_js_install_output(os64_js_runtime_t *rt, int32_t handle, uint32_t names,
                                        os64_js_outcome_t *outcome)
{
    record("output");
    uint32_t known = OS64_JS_OUTPUT_PRINT | OS64_JS_OUTPUT_CONSOLE_LOG;
    if (bad_runtime(rt) || names == 0 || (names & ~known) || js_fake.installed_output ||
        js_fake.evaluated)
        return answer(&(js_fake_answer_t){.status = OS64_JS_BAD_ARGUMENT}, outcome);
    js_fake.installed_output = 1;
    js_fake.output_handle = handle;
    js_fake.output_names = names;
    if (js_fake.output.status != OS64_JS_OK)
        return answer(&js_fake.output, outcome);
    begin(outcome, OS64_JS_OK);
    return OS64_JS_OK;
}

os64_js_status_t os64_js_install_args(os64_js_runtime_t *rt, size_t count,
                                      const char *const *args, os64_js_outcome_t *outcome)
{
    record("args");
    if (bad_runtime(rt) || (count && args == NULL) || js_fake.installed_args || js_fake.evaluated)
        return answer(&(js_fake_answer_t){.status = OS64_JS_BAD_ARGUMENT}, outcome);
    if (count > JS_FAKE_MAX_ARGS)
        abort();
    for (size_t i = 0; i < count; i++) {
        if (args[i] == NULL)
            return answer(&(js_fake_answer_t){.status = OS64_JS_BAD_ARGUMENT}, outcome);
        // Copied, as the contract promises: the caller may release them now.
        strncpy(js_fake.args[i], args[i], sizeof(js_fake.args[i]) - 1);
        js_fake.args[i][sizeof(js_fake.args[i]) - 1] = 0;
    }
    js_fake.arg_count = count;
    js_fake.installed_args = 1;
    begin(outcome, OS64_JS_OK);
    return OS64_JS_OK;
}

static os64_js_status_t evaluate(os64_js_runtime_t *rt, os64_js_outcome_t *outcome,
                                 const char *source_name)
{
    if (bad_runtime(rt))
        return answer(&(js_fake_answer_t){.status = OS64_JS_BAD_ARGUMENT}, outcome);
    js_fake.evaluated = 1;
    os64_js_status_t s = answer(&js_fake.run, outcome);
    // The library copies the source name into every outcome it reports.
    if (!outcome->source_name[0] && source_name)
        strncpy(outcome->source_name, source_name, sizeof(outcome->source_name) - 1);
    return s;
}

os64_js_status_t os64_js_run(os64_js_runtime_t *rt, const void *source, size_t length,
                             const char *source_name, os64_js_outcome_t *outcome)
{
    record("run");
    if (source == NULL && length != 0)
        return answer(&(js_fake_answer_t){.status = OS64_JS_BAD_ARGUMENT}, outcome);
    js_fake.source_length = length;
    js_fake.source_copy_ok = length < sizeof(js_fake.source);
    if (js_fake.source_copy_ok) {
        memcpy(js_fake.source, source, length);
        js_fake.source[length] = 0;
    }
    strncpy(js_fake.source_name, source_name ? source_name : "", sizeof(js_fake.source_name) - 1);
    // The source budget is the library's verdict, checked before copying.
    if (length > js_fake.config.limits.source_bytes)
        return answer(&(js_fake_answer_t){.status = OS64_JS_LIMIT,
                                          .limit = OS64_JS_LIMIT_SOURCE}, outcome);
    return evaluate(rt, outcome, source_name);
}

os64_js_status_t os64_js_run_file(os64_js_runtime_t *rt, const char *path,
                                  os64_js_outcome_t *outcome)
{
    record("run_file");
    strncpy(js_fake.path, path ? path : "", sizeof(js_fake.path) - 1);
    return evaluate(rt, outcome, path);
}

os64_js_status_t os64_js_eval(os64_js_runtime_t *rt, const void *source, size_t length,
                              const char *source_name, os64_js_outcome_t *outcome)
{
    (void)rt; (void)source; (void)length; (void)source_name;
    record("eval");   // the runner never evaluates without draining
    return answer(&(js_fake_answer_t){.status = OS64_JS_BAD_ARGUMENT}, outcome);
}

os64_js_status_t os64_js_drain_jobs(os64_js_runtime_t *rt, uint64_t slice_jobs,
                                    os64_js_outcome_t *outcome)
{
    (void)rt; (void)slice_jobs;
    record("drain");  // nor carries its own job loop
    return answer(&(js_fake_answer_t){.status = OS64_JS_BAD_ARGUMENT}, outcome);
}

void os64_js_cancel(os64_js_runtime_t *rt)
{
    (void)rt;
    record("cancel");
}

void os64_js_destroy(os64_js_runtime_t *rt)
{
    if (rt == NULL)
        return;
    record("destroy");
    if (bad_runtime(rt))
        abort();      // a double destroy would be a runner defect
    s_runtime.alive = 0;
}

void js_fake_reset(void)
{
    memset(&js_fake, 0, sizeof(js_fake));
    s_runtime.alive = 0;
}
