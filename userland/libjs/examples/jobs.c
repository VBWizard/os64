#include "os64/js.h"

os64_js_status_t js_example_expression(os64_js_runtime_t *runtime,
                                      os64_js_outcome_t *outcome)
{
    const char source[] = "Promise.resolve(21).then(x => console.log(x * 2));";
    os64_js_status_t status = os64_js_eval(runtime, source, sizeof(source) - 1,
                                          "expression", outcome);
    while ((status == OS64_JS_OK && outcome->jobs_pending) ||
           status == OS64_JS_MORE_JOBS)
        status = os64_js_drain_jobs(runtime, 1, outcome);
    return status;
}
