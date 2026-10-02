#include "os64/os64.h"
static unsigned checks, failures;
static void check(int passed, const char *name)
{
    checks++;
    if (!passed) { failures++; os64_printf("FAIL: %s\n", name); }
}
#include "cases.h"

static void file_helpers(int32_t output_handle)
{
    const char *path = "/home/jsr2-source.js";
    char source[12000];
    os64_memset(source, ' ', sizeof(source));
    const char *program = "print('file');Promise.resolve().then(()=>globalThis.fileDone=42);";
    os64_memcpy(source, program, os64_strlen(program));
    int64_t handle = os64_open(path, "w");
    check(handle >= 0, "guest file source opened");
    if (handle < 0) return;
    check(os64_write((int32_t)handle, source, sizeof(source)) == sizeof(source), "guest large source written");
    check(os64_close((int32_t)handle) == 0, "guest source committed");
    os64_js_config_t config = fixture_config();
    os64_js_runtime_t *runtime = create_fixture(&config);
    if (!runtime) return;
    os64_js_outcome_t outcome;
    check(os64_js_install_output(runtime, output_handle, OS64_JS_OUTPUT_PRINT, &outcome) == OS64_JS_OK,
          "guest file output setup");
    check(os64_js_run_file(runtime, path, &outcome) == OS64_JS_OK && outcome.jobs_executed == 1,
          "guest bounded file evaluation and Promise draining");
    check(execute_fixture(runtime, "if(fileDone!==42)throw Error('file job')", &outcome) == OS64_JS_OK,
          "guest file context retains job result");
    os64_js_destroy(runtime);
    handle = os64_open(path, "w");
    check(handle >= 0 && os64_close((int32_t)handle) == 0, "guest empty source file created");
    runtime = create_fixture(&config);
    if (!runtime) return;
    check(os64_js_run_file(runtime, path, &outcome) == OS64_JS_OK, "guest empty file runs");
    os64_js_destroy(runtime);
    runtime = create_fixture(&config);
    if (!runtime) return;
    check(os64_js_run_file(runtime, "/home/jsr2-missing.js", &outcome) == OS64_JS_HOST_FAILURE && outcome.host_error < 0,
          "guest missing file preserves open error");
    os64_js_destroy(runtime);
}

static void guest_helpers(void)
{
    const char *path = "/home/jsr2-helper-output.txt";
    int64_t handle = os64_open(path, "w");
    check(handle >= 0, "guest borrowed output opened");
    if (handle < 0) return;
    helper_cases((int32_t)handle);
    output_transaction_cases((int32_t)handle);
    file_helpers((int32_t)handle);
    check(os64_close((int32_t)handle) == 0, "host closes borrowed output after runtime destruction");
    handle = os64_open(path, "r");
    check(handle >= 0, "guest output reopened for comparison");
    if (handle < 0) return;
    char returned[128];
    int64_t n = os64_read((int32_t)handle, returned, sizeof(returned));
    const char expected[] = "hi 7 a\0b\n\nconverted\nfile\n";
    check(n == sizeof(expected)-1 && os64_memcmp(returned, expected, sizeof(expected)-1) == 0,
          "guest output spacing, embedded NUL, conversion and file output bytes");
    check(os64_close((int32_t)handle) == 0, "guest comparison input closed");
}

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
    runtime_cases(); guest_helpers();
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
