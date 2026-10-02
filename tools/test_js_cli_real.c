// test_js_cli_real.c — the js runner against the REAL JavaScript library.
//
// Where test_js_cli_host.c holds the runner's own decisions to USAGE.md
// against a stand-in, this suite runs actual JavaScript through the whole
// stack: the runner's main (as js_main), Quinn's runtime (libjs/runtime), the
// cross-built QuickJS core and libmath's cross-built objects. Only the
// operating system is the host's: files are real files in a scratch
// directory, the monotonic clock is the host's (so a time budget really
// elapses), libos64's calendar is real (via test_js_port_calendar.c), and
// handles 0, 1 and 2 belong to this file. It proves the runner and the
// library agree; the guest run proves the same on os64.
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "os64/date.h"
#include "os64/dirent.h"

int js_main(int argc, char **argv);

// ---- the os64 services the runner and the library reach ----

os64_time_t js_test_clock = {.epoch = 1700000000, .ticks_per_second = 100};
int js_test_clock_failed;

typedef union { max_align_t align; size_t size; } block_t;
static size_t s_live;
void *os64_malloc(size_t n)
{
    n = (n + 15) & ~(size_t)15;
    block_t *b = malloc(sizeof(block_t) + (n ? n : 16));
    if (!b) return NULL;
    b->size = n ? n : 16;
    s_live++;
    return b + 1;
}
size_t os64_malloc_size(const void *p) { return ((const block_t *)p)[-1].size; }
void os64_free(void *p) { if (p) { s_live--; free((block_t *)p - 1); } }
void *os64_realloc(void *p, size_t n)
{
    if (!p) return os64_malloc(n);
    if (!n) { os64_free(p); return NULL; }
    void *q = os64_malloc(n);
    if (q) { size_t old = os64_malloc_size(p); memcpy(q, p, old < n ? old : n); os64_free(p); }
    return q;
}

static char s_out[65536], s_err[65536];
static size_t s_out_len, s_err_len;
static int64_t s_out_error;        // nonzero: every stdout write fails
static const char *s_stdin;
static size_t s_stdin_len, s_stdin_at;

int64_t os64_write(int32_t handle, const void *data, size_t n)
{
    if (handle == 1 && s_out_error) return s_out_error;
    char *dst = handle == 1 ? s_out : s_err;
    size_t *len = handle == 1 ? &s_out_len : &s_err_len;
    if (handle != 1 && handle != 2) abort();
    if (*len + n >= sizeof(s_out)) n = sizeof(s_out) - 1 - *len;
    memcpy(dst + *len, data, n);
    *len += n;
    dst[*len] = 0;
    return (int64_t)n;
}

// Host descriptors, shifted clear of 0/1/2 so a confusion cannot pass.
#define HANDLE_BASE 1000
int64_t os64_open(const char *path, const char *mode)
{
    if (strcmp(mode, "r")) abort();
    int fd = open(path, O_RDONLY);
    return fd < 0 ? -2 : HANDLE_BASE + fd;
}
int64_t os64_read(int32_t handle, void *buf, size_t len)
{
    if (handle == 0) {
        size_t n = s_stdin_len - s_stdin_at;
        if (n > len) n = len;
        memcpy(buf, s_stdin + s_stdin_at, n);
        s_stdin_at += n;
        return (int64_t)n;
    }
    ssize_t n = read(handle - HANDLE_BASE, buf, len);
    return n < 0 ? -5 : n;
}
int64_t os64_close(int32_t handle) { return close(handle - HANDLE_BASE) ? -5 : 0; }
int64_t os64_stat(const char *path, os64_dirent_t *entry)
{
    struct stat st;
    if (stat(path, &st)) return -1;
    memset(entry, 0, sizeof(*entry));
    entry->flags = S_ISDIR(st.st_mode) ? OS64_DE_DIR : 0;
    return 0;
}
// No environment: dates are UTC whatever the host's TZ says.
const char *os64_getenv(const char *name) { (void)name; return NULL; }

int64_t os64_micros(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
void os64_exit(int32_t status)
{
    fprintf(stderr, "os64_exit(0x%08x)\n", (unsigned)status);
    _Exit(99);
}

// ---- cases ----

static unsigned s_checks, s_failures;
static const char *s_case;
static char s_dir[256];

static void check(int ok, const char *what)
{
    s_checks++;
    if (!ok) {
        s_failures++;
        fprintf(stderr, "FAIL [%s] %s\n  stdout: %s\n  stderr: %s\n", s_case, what, s_out, s_err);
    }
}

static const char *file(const char *name, const char *contents)
{
    static char path[512];
    snprintf(path, sizeof(path), "%s/%s", s_dir, name);
    FILE *f = fopen(path, "w");
    fputs(contents, f);
    fclose(f);
    return path;
}

// Run js with the given words (NULL-terminated) and standard input.
static int js(const char *name, const char *in, const char *const *words)
{
    char *argv[32];
    int argc = 0;
    argv[argc++] = "js";
    while (*words) argv[argc++] = (char *)*words++;
    argv[argc] = NULL;
    s_case = name;
    s_out_len = s_err_len = 0;
    s_out[0] = s_err[0] = 0;
    s_stdin = in ? in : "";
    s_stdin_len = strlen(s_stdin);
    s_stdin_at = 0;
    s_live = 0;
    int status = js_main(argc, argv);
    check(s_live == 0, "the runner and the library release every allocation");
    // JS_CLI_VERBOSE=1 shows what each case printed: the way to read the
    // library's real diagnostics rather than trust that a substring matched.
    if (getenv("JS_CLI_VERBOSE"))
        fprintf(stderr, "== %s: exit %d\n-- stdout:\n%s-- stderr:\n%s", name, status, s_out, s_err);
    return status;
}
#define JS(name, in, ...) js(name, in, (const char *const[]){__VA_ARGS__, NULL})

static int has(const char *s, const char *t) { return strstr(s, t) != NULL; }

int main(void)
{
    snprintf(s_dir, sizeof(s_dir), "/tmp/js-cli-real-XXXXXX");
    if (!mkdtemp(s_dir)) return 2;

    const char *tool = file("tool.js", "print('hi', 1 + 1); console.log(JSON.stringify(scriptArgs));\n");
    char path[512];
    strcpy(path, tool);
    int st = JS("file", NULL, path, "a", "-v");
    char want[1024];
    snprintf(want, sizeof(want), "hi 2\n[\"%s\",\"a\",\"-v\"]\n", path);
    check(st == 0 && !strcmp(s_out, want) && !s_err[0], "a file runs; print and console.log reach stdout; scriptArgs");

    st = JS("eval", NULL, "-e", "print(6 * 7, scriptArgs.length, scriptArgs[0])");
    check(st == 0 && !strcmp(s_out, "42 1 -e\n"), "-e runs SOURCE");

    st = JS("stdin", "print(scriptArgs[0], scriptArgs[1], 6 * 7)", "-", "x");
    check(st == 0 && !strcmp(s_out, "- x 42\n"), "- runs standard input");

    st = JS("jobs drained", NULL, "-e",
            "Promise.resolve(2).then(x => print(x * 21)); (async () => { await 1; print('after') })()");
    check(st == 0 && !strcmp(s_out, "42\nafter\n"), "Promise jobs run to completion before exit");

    st = JS("maths", NULL, "-e", "print(Math.acosh(-49152), Math.sin(1e300), Math.round(-2.5))");
    check(st == 0 && !strcmp(s_out, "NaN -0.8178819121159085 -2\n"), "libmath underneath, with JavaScript's own rounding");

    st = JS("throw", NULL, "-e", "function f() { throw new TypeError('nope') }\nf()");
    check(st == 1 && has(s_err, "js: -e: TypeError: nope\n") && has(s_err, "at f"),
          "an uncaught exception exits 1 with its message and stack");

    st = JS("syntax", NULL, "-e", "let = ;");
    check(st == 1 && has(s_err, "js: -e: SyntaxError"), "a malformed script exits 1 naming the SyntaxError");

    st = JS("rejection", NULL, "-e", "Promise.reject(new Error('boom'))");
    check(st == 1 && has(s_err, "js: -e: unhandled promise rejection: boom\n"),
          "an unhandled rejection exits 1");

    st = JS("handled rejection", NULL, "-e", "Promise.reject(new Error('boom')).catch(e => print('caught', e.message))");
    check(st == 0 && !strcmp(s_out, "caught boom\n"), "a rejection handled in a later job is not reported");

    st = JS("time", NULL, "-t", "200", "-e", "for (;;) {}");
    check(st == 4 && !strcmp(s_err, "js: -e: time budget exceeded (200 ms)\n"), "the time budget");

    st = JS("memory", NULL, "-m", "2M", "-e", "let a = []; for (;;) a.push('x'.repeat(1000) + a.length)");
    check(st == 4 && !strcmp(s_err, "js: -e: memory budget exceeded (2M)\n"), "the memory budget");

    st = JS("jobs", NULL, "--jobs", "5", "-e", "function f() { Promise.resolve().then(f) } f()");
    check(st == 4 && !strcmp(s_err, "js: -e: jobs budget exceeded (5 jobs)\n"), "the jobs budget");

    const char *big = file("big.js", "print('this script is longer than ten bytes')\n");
    strcpy(path, big);
    st = JS("source", NULL, "--source", "10", path);
    check(st == 4 && has(s_err, "source budget exceeded (10 bytes)"), "the source budget, on a file");
    st = JS("source stdin", "print('this script is longer than ten bytes')", "--source", "10", "-");
    check(st == 4 && has(s_err, "js: -: source budget exceeded (10 bytes)"), "the source budget, on standard input");

    // The library reports LIMIT/STACK only on a reliable engine signal
    // (CONTRACT.md § Results), and QuickJS's is an ordinary exception.
    st = JS("recursion", NULL, "-e", "function r() { return r() + 1 } r()");
    check(st == 1 && has(s_err, "js: -e: InternalError: stack overflow\n    at r") &&
          has(s_err, "    [truncated]\n"),
          "runaway recursion is an exception, not a fault, and its long trace is marked cut");

    snprintf(path, sizeof(path), "%s/nosuch.js", s_dir);
    st = JS("missing", NULL, path);
    snprintf(want, sizeof(want), "js: %s: no such file\n", path);
    check(st == 3 && !strcmp(s_err, want), "a missing file");
    st = JS("directory", NULL, s_dir);
    snprintf(want, sizeof(want), "js: %s: is a directory\n", s_dir);
    check(st == 3 && !strcmp(s_err, want), "a directory");

    s_out_error = -32;
    st = JS("output", NULL, "-e", "print('nobody hears this')");
    s_out_error = 0;
    check(st == 3 && has(s_err, "js: -e: ") && has(s_err, "(error -32)"),
          "a failed write to standard output exits 3 with its error");

    st = JS("usage", NULL, "-m", "0", "-e", "1");
    check(st == 2, "usage still exits 2 with the real library linked");

    printf("js runner on the real library: %u checks, %u failures\n", s_checks, s_failures);
    char cleanup[600];
    snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", s_dir);
    if (system(cleanup) != 0) return 2;
    return s_failures != 0;
}
