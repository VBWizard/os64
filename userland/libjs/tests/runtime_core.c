#include "os64/os64.h"
static unsigned checks, failures;
static void check(int passed, const char *name)
{
    checks++;
    if (!passed) { failures++; os64_printf("FAIL: %s\n", name); }
}
#include "cases.h"

int main(int argc, char **argv)
{
    if (argc == 2) {
        os64_js_config_t config = fixture_config();
        fixture_runtime = create_fixture(&config);
        if (fixture_runtime == NULL) return 1;
        os64_js_outcome_t outcome;
        if (os64_streq(argv[1], "--leak")) {
            JSContext *context = os64_js_context(fixture_runtime, OS64_JS_ABI_ID, &outcome);
            (void)JS_NewObject(context);
            os64_js_destroy(fixture_runtime);
        } else if (os64_streq(argv[1], "--active-destroy")) {
            install_native(fixture_runtime, "destroy", native_destroy);
            execute_fixture(fixture_runtime, "destroy()", &outcome);
        }
        return 1;
    }
    runtime_cases();
    os64_js_config_t config = fixture_config();
    config.limits.execution_ms = 10;
    os64_js_runtime_t *runtime = create_fixture(&config);
    if (runtime != NULL) {
        os64_js_outcome_t outcome;
        check(execute_fixture(runtime, "try{for(;;){}}catch(e){}", &outcome) == OS64_JS_LIMIT &&
              outcome.limit == OS64_JS_LIMIT_EXECUTION, "guest monotonic deadline interrupts script");
        os64_js_destroy(runtime);
    }
    os64_printf("jsembedtest: %u checks, %u failures, %u native calls\n", checks, failures, native_calls);
    return failures ? 1 : 0x4A535254; /* JSRT */
}
