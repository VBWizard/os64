// test_js_cli_host.c — the js runner's command line, against USAGE.md.
//
// The runner (userland/apps/js/js.c) is compiled for the host with its main
// renamed js_main, beside libos64's real argument parser, formatter and
// string code, and the os64_js_* stand-in in test_js_cli_fake.c. Standard
// output, standard error and standard input are this file's: each case runs
// one command line and checks what was asked of the library, what reached
// each handle, and the exit status.
//
// Every row of USAGE.md's tables that the runner decides has a case here; the
// one it cannot, OS64_JS_FATAL_EXIT, is the library's own exit. When R2
// lands, the same command lines run against the real library
// (JAVASCRIPT_TASKS.md § C1).
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_js_cli_fake.h"

int js_main(int argc, char **argv);

static char s_out[16384], s_err[16384];
static size_t s_out_len, s_err_len;
static const char *s_stdin;
static size_t s_stdin_len, s_stdin_at, s_stdin_chunk;
static int64_t s_stdin_error;      // nonzero: the read after the data fails
static size_t s_stdin_requested;   // total bytes the runner asked to read

// ---- the libos64 surface the runner and its library code reach ----

int64_t os64_write(int32_t handle, const void *data, size_t n)
{
    char *dst = handle == 1 ? s_out : s_err;
    size_t *len = handle == 1 ? &s_out_len : &s_err_len;
    if (*len + n >= sizeof(s_out))
        n = sizeof(s_out) - 1 - *len;
    memcpy(dst + *len, data, n);
    *len += n;
    dst[*len] = 0;
    return (int64_t)n;
}

int64_t os64_read(int32_t handle, void *buf, size_t len)
{
    if (handle != 0) abort();
    s_stdin_requested += len;
    if (s_stdin_at == s_stdin_len)
        return s_stdin_error ? s_stdin_error : 0;
    size_t n = s_stdin_len - s_stdin_at;
    if (n > len) n = len;
    if (s_stdin_chunk && n > s_stdin_chunk) n = s_stdin_chunk;
    memcpy(buf, s_stdin + s_stdin_at, n);
    s_stdin_at += n;
    return (int64_t)n;
}

static size_t s_live;
void *os64_malloc(size_t n) { s_live++; return malloc(n ? n : 1); }
void *os64_realloc(void *p, size_t n) { if (!p) s_live++; return realloc(p, n ? n : 1); }
void os64_free(void *p) { if (p) { s_live--; free(p); } }

// ---- cases ----

static unsigned s_checks, s_failures;
static const char *s_case;

static void check(int ok, const char *what)
{
    s_checks++;
    if (!ok) {
        s_failures++;
        fprintf(stderr, "FAIL [%s] %s\n  stdout: %s\n  stderr: %s\n  calls: %s\n",
                s_case, what, s_out, s_err, js_fake.calls);
    }
}

// Run one command line. `line` is split on spaces; a word wrapped in single
// quotes keeps its spaces (enough shell for these cases, and no more).
static int run(const char *name, const char *line)
{
    static char storage[1024];
    static char *argv[32];
    int argc = 0;
    s_case = name;
    strncpy(storage, line, sizeof(storage) - 1);
    for (char *p = storage; *p;) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '\'') {
            argv[argc++] = ++p;
            while (*p && *p != '\'') p++;
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ') p++;
        }
        if (*p) *p++ = 0;
    }
    argv[argc] = NULL;
    s_out_len = s_err_len = 0;
    s_out[0] = s_err[0] = 0;
    s_stdin_at = s_stdin_requested = 0;
    s_live = 0;
    int status = js_main(argc, argv);
    check(s_live == 0, "every allocation released");
    return status;
}

static void fresh(void)
{
    js_fake_reset();
    s_stdin = "";
    s_stdin_len = 0;
    s_stdin_chunk = 0;
    s_stdin_error = 0;
}

static int has(const char *haystack, const char *needle) { return strstr(haystack, needle) != NULL; }

static void grammar(void)
{
    fresh();
    int st = run("file", "js tool.js a b");
    check(st == 0, "a script that finishes exits 0");
    check(!strcmp(js_fake.calls, "create output args run_file destroy"), "the call sequence");
    check(js_fake.abi_matched, "the runner passes its own ABI identifier");
    check(!strcmp(js_fake.path, "tool.js"), "the file goes to run_file as typed");
    check(js_fake.output_handle == 1, "output is handle 1, so redirection applies");
    check(js_fake.output_names == (OS64_JS_OUTPUT_PRINT | OS64_JS_OUTPUT_CONSOLE_LOG),
          "print and console.log are both granted");
    check(js_fake.arg_count == 3 && !strcmp(js_fake.args[0], "tool.js") &&
          !strcmp(js_fake.args[1], "a") && !strcmp(js_fake.args[2], "b"),
          "scriptArgs is the path then the arguments");
    check(!s_out[0] && !s_err[0], "a clean run prints nothing of its own");

    const os64_js_limits_t *l = &js_fake.config.limits;
    check(l->memory_bytes == 64u << 20 && l->stack_bytes == 256u << 10 &&
          l->source_bytes == 4u << 20 && l->execution_ms == 60000 &&
          l->jobs_per_turn == UINT64_MAX, "the documented defaults");

    fresh();
    run("script flags", "js tool.js -v --out x -m 1");
    check(js_fake.arg_count == 6 && !strcmp(js_fake.args[1], "-v") &&
          !strcmp(js_fake.args[2], "--out") && !strcmp(js_fake.args[5], "1") &&
          js_fake.config.limits.memory_bytes == 64u << 20,
          "everything after the file is the script's, flags included");

    fresh();
    st = run("eval", "js -e 'print(6 * 7)' a");
    check(st == 0 && !strcmp(js_fake.calls, "create output args run destroy"), "-e runs source");
    check(!strcmp(js_fake.source, "print(6 * 7)") && js_fake.source_length == 12,
          "-e hands SOURCE over exactly, without its terminator");
    check(!strcmp(js_fake.source_name, "-e"), "-e's source is named -e");
    check(js_fake.arg_count == 2 && !strcmp(js_fake.args[0], "-e") && !strcmp(js_fake.args[1], "a"),
          "-e's scriptArgs start with -e");

    fresh();
    run("eval long", "js --eval=1 x");
    check(!strcmp(js_fake.source, "1") && js_fake.arg_count == 2, "--eval=SOURCE");

    fresh();
    run("eval stops options", "js -e 1 -m 5");
    check(js_fake.arg_count == 3 && !strcmp(js_fake.args[1], "-m") &&
          js_fake.config.limits.memory_bytes == 64u << 20,
          "options after -e SOURCE are the script's");

    fresh();
    s_stdin = "print(scriptArgs)";
    s_stdin_len = strlen(s_stdin);
    s_stdin_chunk = 5;
    st = run("stdin", "js - a b");
    check(st == 0 && !strcmp(js_fake.calls, "create output args run destroy"), "- runs stdin");
    check(!strcmp(js_fake.source, "print(scriptArgs)"), "short reads reassemble the script");
    check(!strcmp(js_fake.source_name, "-") && !strcmp(js_fake.args[0], "-") &&
          js_fake.arg_count == 3, "stdin's script is named -");

    fresh();
    st = run("stdin empty", "js -");
    check(st == 0 && js_fake.source_length == 0, "an empty stdin is an empty script");

    fresh();
    run("dashdash", "js -- -weird.js z");
    check(!strcmp(js_fake.path, "-weird.js") && js_fake.arg_count == 2,
          "-- lets a file name start with -");

    fresh();
    run("budgets", "js -m 1M -t 500 --stack 128K --source 2K --jobs 10 x.js");
    l = &js_fake.config.limits;
    check(l->memory_bytes == 1u << 20 && l->execution_ms == 500 && l->stack_bytes == 128u << 10 &&
          l->source_bytes == 2u << 10 && l->jobs_per_turn == 10, "every budget option lands");

    fresh();
    run("budgets inline", "js --memory=2m --time=7 --stack=768K --source=100 --jobs=1 x.js");
    l = &js_fake.config.limits;
    check(l->memory_bytes == 2u << 20 && l->execution_ms == 7 && l->stack_bytes == 768u << 10 &&
          l->source_bytes == 100 && l->jobs_per_turn == 1, "--name=value forms, lower-case suffix, the stack cap itself");

    fresh();
    st = run("help", "js -h");
    check(st == 0 && has(s_out, "usage") && !js_fake.calls[0], "-h prints usage and runs nothing");
}

static void usage_errors(void)
{
    static const struct { const char *line, *says; } cases[] = {
        { "js", "no script" },
        { "js -z x.js", "unknown option" },
        { "js --nope x.js", "unknown option" },
        { "js -e", "missing value" },
        { "js -m 0 x.js", "--memory" },
        { "js -m 12Q x.js", "--memory" },
        { "js -m M x.js", "--memory" },
        { "js -m 99999999999999999999 x.js", "--memory" },
        { "js -m 17592186044416M x.js", "--memory" },
        { "js -t 0 x.js", "--time" },
        { "js -t 1s x.js", "--time" },
        { "js --stack 769K x.js", "--stack" },
        { "js --stack 1M x.js", "--stack" },
        { "js --source 0 x.js", "--source" },
        { "js --jobs 0 x.js", "--jobs" },
        { "js --jobs -1 x.js", "--jobs" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fresh();
        int st = run(cases[i].line, cases[i].line);
        check(st == 2, "a usage error exits 2");
        check(!js_fake.calls[0], "a usage error never reaches the library");
        check(has(s_err, cases[i].says), "the message names the problem");
        check(has(s_out, "usage"), "and the usage follows");
    }
}

static void outcome(const char *name, const char *line, js_fake_answer_t answer,
                    int want_status, const char *want_err)
{
    fresh();
    js_fake.run = answer;
    int st = run(name, line);
    check(st == want_status, "the documented exit status");
    check(has(s_err, want_err), "the documented diagnostic");
    check(has(js_fake.calls, "destroy"), "the runtime is destroyed");
}

static void outcomes(void)
{
    outcome("exception", "js tool.js",
            (js_fake_answer_t){.status = OS64_JS_EXCEPTION, .line = 12, .column = 5,
                               .message = "TypeError: not a function",
                               .stack_trace = "    at main (tool.js:12:5)"},
            1, "js: tool.js:12:5: TypeError: not a function\n    at main (tool.js:12:5)\n");
    outcome("exception, no location", "js -e 'throw 1'",
            (js_fake_answer_t){.status = OS64_JS_EXCEPTION, .message = "1"}, 1, "js: -e: 1\n");
    outcome("rejection", "js tool.js",
            (js_fake_answer_t){.status = OS64_JS_UNHANDLED_REJECTION, .message = "Error: boom"},
            1, "js: tool.js: unhandled promise rejection: Error: boom\n");
    outcome("memory", "js tool.js",
            (js_fake_answer_t){.status = OS64_JS_LIMIT, .limit = OS64_JS_LIMIT_MEMORY},
            4, "js: tool.js: memory budget exceeded (64M)\n");
    outcome("time", "js tool.js",
            (js_fake_answer_t){.status = OS64_JS_LIMIT, .limit = OS64_JS_LIMIT_EXECUTION},
            4, "js: tool.js: time budget exceeded (60000 ms)\n");
    outcome("jobs", "js --jobs 10 tool.js",
            (js_fake_answer_t){.status = OS64_JS_LIMIT, .limit = OS64_JS_LIMIT_JOBS},
            4, "js: tool.js: jobs budget exceeded (10 jobs)\n");
    outcome("stack", "js --stack 100000 tool.js",
            (js_fake_answer_t){.status = OS64_JS_LIMIT, .limit = OS64_JS_LIMIT_STACK},
            4, "js: tool.js: stack budget exceeded (100000 bytes)\n");
    outcome("source", "js tool.js",
            (js_fake_answer_t){.status = OS64_JS_LIMIT, .limit = OS64_JS_LIMIT_SOURCE},
            4, "js: tool.js: source budget exceeded (4M)\n");
    outcome("missing file", "js nosuch.js",
            (js_fake_answer_t){.status = OS64_JS_HOST_FAILURE, .host_error = -2,
                               .message = "cannot read"},
            3, "js: nosuch.js: cannot read (error -2)\n");
    outcome("host failure, no message", "js tool.js",
            (js_fake_answer_t){.status = OS64_JS_HOST_FAILURE, .host_error = -9},
            3, "js: tool.js: input or output failed (error -9)\n");
    outcome("truncated", "js tool.js",
            (js_fake_answer_t){.status = OS64_JS_EXCEPTION, .message = "Error: long",
                               .truncated = true},
            1, "js: tool.js: Error: long [truncated]\n");
    outcome("library names the source", "js tool.js",
            (js_fake_answer_t){.status = OS64_JS_EXCEPTION, .line = 3, .column = 1,
                               .message = "Error: in a job", .source_name = "lib.js"},
            1, "js: lib.js:3:1: Error: in a job\n");
    outcome("cancelled", "js tool.js",
            (js_fake_answer_t){.status = OS64_JS_CANCELLED}, 5,
            "js: tool.js: the JavaScript library answered CANCELLED\n");
    outcome("busy", "js tool.js",
            (js_fake_answer_t){.status = OS64_JS_BUSY, .message = "turn active"}, 5,
            "js: tool.js: the JavaScript library answered BUSY: turn active\n");

    // Refusals before evaluation: nothing runs, and creation's failure leaves
    // no runtime to destroy.
    fresh();
    js_fake.create = (js_fake_answer_t){.status = OS64_JS_ABI_MISMATCH};
    int st = run("abi", "js tool.js");
    check(st == 5 && !strcmp(js_fake.calls, "create") && has(s_err, "ABI_MISMATCH"),
          "a refused creation exits 5 and runs nothing");
    fresh();
    js_fake.create = (js_fake_answer_t){.status = OS64_JS_LIMIT, .limit = OS64_JS_LIMIT_MEMORY};
    st = run("create memory", "js -m 1K tool.js");
    check(st == 4 && has(s_err, "memory budget exceeded (1K)"), "a budget too small to start is a budget error");
    fresh();
    js_fake.output = (js_fake_answer_t){.status = OS64_JS_HOST_FAILURE, .host_error = -32};
    st = run("output", "js tool.js");
    check(st == 3 && !strcmp(js_fake.calls, "create output destroy"),
          "a failed installer stops before evaluation and still destroys");
}

static void standard_input(void)
{
    // The runner reads at most one byte past the source budget, and lets the
    // library refuse it, whatever the stream holds.
    static char big[5000];
    memset(big, 'x', sizeof(big));
    fresh();
    s_stdin = big;
    s_stdin_len = sizeof(big);
    int st = run("stdin over budget", "js --source 1000 -");
    check(st == 4 && js_fake.source_length == 1001 && has(s_err, "source budget exceeded (1000 bytes)"),
          "one byte over the budget is enough for the library's verdict");
    check(s_stdin_at == 1001, "and no more of the stream is consumed");

    fresh();
    s_stdin = "print(1)";
    s_stdin_len = 8;
    s_stdin_error = -5;
    st = run("stdin error", "js -");
    check(st == 3 && !js_fake.calls[0] && has(s_err, "js: -: cannot read standard input (error -5)"),
          "a failed read of standard input exits 3 before the library is asked");

    fresh();
    s_stdin = big;
    s_stdin_len = 1000;
    s_stdin_chunk = 1;
    st = run("stdin exact budget", "js --source 1000 -");
    check(st == 0 && js_fake.source_length == 1000, "a script exactly at the budget runs");
}

int main(void)
{
    grammar();
    usage_errors();
    outcomes();
    standard_input();
    printf("js runner: %u checks, %u failures\n", s_checks, s_failures);
    return s_failures != 0;
}
