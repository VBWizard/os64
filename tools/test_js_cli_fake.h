// test_js_cli_fake.h — the scripting and recording surface of the
// os64_js_* stand-in (test_js_cli_fake.c).
#ifndef TEST_JS_CLI_FAKE_H
#define TEST_JS_CLI_FAKE_H
#include <stddef.h>
#include "os64/js.h"

#define JS_FAKE_MAX_ARGS 16

// What one operation answers. Zero is OK with no diagnostics.
typedef struct {
    os64_js_status_t status;
    os64_js_limit_t limit;
    int64_t host_error;
    uint32_t line, column;
    bool truncated;
    char message[OS64_JS_MESSAGE_CAP];
    char stack_trace[OS64_JS_STACK_TRACE_CAP];
    char source_name[OS64_JS_SOURCE_NAME_CAP];   // empty: the call's own name
} js_fake_answer_t;

typedef struct {
    // scripted
    js_fake_answer_t create, output, run;
    // recorded
    char calls[256];                 // "create output args run destroy"
    os64_js_config_t config;
    bool abi_matched;
    int32_t output_handle;
    uint32_t output_names;
    char args[JS_FAKE_MAX_ARGS][128];
    size_t arg_count;
    char path[256];
    char source[8192];
    size_t source_length;
    bool source_copy_ok;
    char source_name[128];
    // contract state
    int installed_output, installed_args, evaluated;
} js_fake_t;

extern js_fake_t js_fake;
void js_fake_reset(void);
#endif
