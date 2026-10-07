#include "os64/os64.h"
static unsigned checks, failures;
static void check(int passed, const char *name)
{
    checks++;
    if (!passed) { failures++; os64_printf("FAIL: %s\n", name); }
}
#include "cases.h"
static bool teardown_guest_heap(uint64_t *bytes, uint64_t *blocks)
{
    char text[4096];
    int64_t handle = os64_open("/proc/self/heap", "r");
    if (handle < 0) return false;
    int64_t length = os64_read((int32_t)handle, text, sizeof(text) - 1);
    int64_t closed = os64_close((int32_t)handle);
    if (length <= 0 || closed != 0) return false;
    text[length] = 0;
    const char *names[] = {"live\t", "blocks_live\t"};
    uint64_t *values[] = {bytes, blocks};
    for (unsigned i = 0; i < 2; i++) {
        const char *value = fixture_contains(text, names[i]);
        if (value == NULL || (value != text && value[-1] != '\n')) return false;
        value += os64_strlen(names[i]);
        if (*value < '0' || *value > '9') return false;
        *values[i] = 0;
        while (*value >= '0' && *value <= '9') {
            if (*values[i] > (UINT64_MAX - (unsigned)(*value - '0')) / 10) return false;
            *values[i] = *values[i] * 10 + (unsigned)(*value++ - '0');
        }
        if (*value != '\n') return false;
    }
    return true;
}
#define JS_TEARDOWN_GUEST
#include "teardown.h"

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
    for (size_t capacity = 4096; capacity <= 8192; capacity *= 2) {
        config = file_boundary_config(capacity);
        for (size_t extra = 0; extra <= 1; extra++) {
            size_t length = capacity - 1 + extra;
            os64_memset(source, ' ', length);
            os64_memcpy(source, "true", 4);
            handle = os64_open(path, "w");
            check(handle >= 0, "guest boundary source opened");
            if (handle < 0) return;
            check(os64_write((int32_t)handle, source, length) == (int64_t)length,
                  "guest boundary source written");
            check(os64_close((int32_t)handle) == 0, "guest boundary source committed");
            runtime = create_fixture(&config);
            if (!runtime) return;
            check(os64_js_run(runtime, source, length, path, &outcome) == OS64_JS_OK,
                  "guest boundary buffer evaluation fits memory budget");
            os64_js_destroy(runtime);
            runtime = create_fixture(&config);
            if (!runtime) return;
            os64_js_status_t result = os64_js_run_file(runtime, path, &outcome);
            check(extra == 0 ? result == OS64_JS_OK : result == OS64_JS_LIMIT &&
                  outcome.limit == OS64_JS_LIMIT_MEMORY,
                  "guest EOF boundary avoids growth but extra input requires it");
            os64_js_destroy(runtime);
        }
    }
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
    teardown_cases(); runtime_cases(); guest_helpers();
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
