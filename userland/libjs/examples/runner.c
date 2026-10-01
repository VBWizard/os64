#include "os64/js.h"

/* Explicit development budgets for an API example, not selected production
 * defaults. Diagnostics can be presented after this call by the application. */
int js_example_run_file(const char *path, os64_js_outcome_t *result)
{
    os64_js_config_t config = {{16 * 1024 * 1024, 256 * 1024,
                               1024 * 1024, 1000, 10000}};
    os64_js_runtime_t *runtime = NULL;
    os64_js_status_t status = os64_js_create(&config, OS64_JS_ABI_ID,
                                            &runtime, result);
    if (status != OS64_JS_OK) return 1;
    const char *args[] = {path};
    status = os64_js_install_output(runtime, 1,
        OS64_JS_OUTPUT_PRINT | OS64_JS_OUTPUT_CONSOLE_LOG, result);
    if (status == OS64_JS_OK)
        status = os64_js_install_args(runtime, 1, args, result);
    if (status == OS64_JS_OK)
        status = os64_js_run_file(runtime, path, result);
    os64_js_destroy(runtime);
    return status == OS64_JS_OK ? 0 : 1;
}
