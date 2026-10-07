#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdalign.h>
#include <limits.h>
#include "allocator.h"
#include "platform.h"
#include "os64/date.h"
#include "os64/js.h"

static unsigned checks, failures;
static size_t live, live_bytes, attempts, fail_at;
os64_time_t js_test_clock = {.epoch = 1700000000, .tz_offset_minutes = 330,
                            .ticks_into_second = 37, .ticks_per_second = 100};
int js_test_clock_failed;
static char output[16384];
static size_t output_len, write_limit = SIZE_MAX;
static int write_failed;

typedef union { max_align_t alignment; struct { size_t size; } meta; } Block;
void *os64_malloc(size_t n)
{
    if (++attempts == fail_at || n > SIZE_MAX - 15 - sizeof(Block)) return NULL;
    n = (n + 15) & ~(size_t)15;
    Block *block = malloc(sizeof(*block) + n);
    if (!block) return NULL;
    block->meta.size = n; live++; live_bytes += n;
    return block + 1;
}
size_t os64_malloc_size(const void *p) { return ((const Block *)p)[-1].meta.size; }
void os64_free(void *p) { if (p) { live--; live_bytes -= os64_malloc_size(p); free((Block *)p - 1); } }
void *os64_realloc(void *p, size_t n)
{
    if (!p) return os64_malloc(n);
    if (!n) { os64_free(p); return NULL; }
    void *next = os64_malloc(n);
    if (next) { size_t old = os64_malloc_size(p); memcpy(next, p, n < old ? n : old); os64_free(p); }
    return next;
}
const char *os64_getenv(const char *key) { return getenv(key); }
int64_t os64_write(int32_t fd, const void *data, size_t n)
{
    if (write_failed) return -1;
    if (n > write_limit) n = write_limit;
    if (fd == 2) return (int64_t)fwrite(data, 1, n, stderr);
    if (n > sizeof(output) - output_len) return -1;
    memcpy(output + output_len, data, n); output_len += n;
    return (int64_t)n;
}
void os64_exit(int32_t status)
{
    fprintf(stderr, "target exit badge: %08x\n", (unsigned)status);
    _Exit(status == OS64_JS_FATAL_EXIT ? 99 : 98);
}
static void check(int passed, const char *name)
{
    checks++;
    if (!passed) { failures++; fprintf(stderr, "FAIL: %s\n", name); }
}
static void allocators(void)
{
    const JSMallocFunctions *f = &jsport_malloc_functions;
    JSMallocState s = {.malloc_limit = 64};
    void *a = f->js_malloc(&s, 17);
    check(a && s.malloc_size == 32 && s.malloc_count == 1, "rounded payload charged");
    memset(a, 0x5a, 17);
    check(!f->js_malloc(&s, SIZE_MAX) && s.malloc_size == 32, "overflow request refused");
    void *b = f->js_realloc(&s, a, 49);
    check(b && s.malloc_size == 64 && s.malloc_count == 1 && ((unsigned char *)b)[16] == 0x5a,
          "growth preserves bytes and accounting");
    s.malloc_limit = 63;
    check(!f->js_realloc(&s, b, 49) && s.malloc_size == 64 && ((unsigned char *)b)[16] == 0x5a,
          "rounding over replacement budget preserves original");
    s.malloc_limit = 128; fail_at = attempts + 1;
    check(!f->js_realloc(&s, b, 70) && s.malloc_size == 64, "failed allocation preserves original");
    fail_at = 0;
    b = f->js_realloc(&s, b, 8);
    check(b && s.malloc_size == 16 && ((unsigned char *)b)[0] == 0x5a, "shrink preserves bytes");
    check(!f->js_realloc(&s, b, 0) && s.malloc_size == 0 && s.malloc_count == 0, "zero resize releases");
    s.malloc_limit = 17;
    check(!f->js_malloc(&s, 17) && !s.malloc_size && !s.malloc_count, "rounding over fresh budget rolls back");
    size_t before = attempts;
    check(!f->js_realloc(&s, NULL, 0) && !f->js_malloc_usable_size(NULL) &&
          attempts == before && !s.malloc_size && !s.malloc_count,
          "null zero resize and usable-size contracts");
    f->js_free(&s, NULL);
    check(live == 0, "allocator fixtures release every block");
    JSPortAllocator owner = {.payload_limit = 17};
    s = (JSMallocState){.malloc_limit = SIZE_MAX, .opaque = &owner};
    check(!f->js_malloc(&s, 17) && owner.failures == JSPORT_ALLOC_LIMIT && live == 0,
          "caller ceiling includes allocator rounding");
    owner.payload_limit = SIZE_MAX; owner.failures = 0; fail_at = attempts + 1;
    check(!f->js_malloc(&s, 17) && owner.failures == JSPORT_ALLOC_OOM && live == 0,
          "OS allocation failure distinguished from ceiling");
    fail_at = 0;
    owner.payload_limit = 1; owner.failures = 0;
    JSRuntime *rt = JS_NewRuntime2(f, &owner);
    check(rt == NULL && owner.failures == JSPORT_ALLOC_LIMIT && live == 0,
          "runtime construction obeys caller ceiling");
    owner.payload_limit = SIZE_MAX; owner.failures = 0; fail_at = attempts + 1;
    rt = JS_NewRuntime2(f, &owner);
    check(rt == NULL && owner.failures == JSPORT_ALLOC_OOM && live == 0,
          "runtime construction reports OS allocation failure");
    fail_at = 0;
}
static void formatting(void)
{
    const char *formats[] = {"%d", "%+07d", "% 8d", "%-8.5d", "%08.5d", "%.0d"};
    int values[] = {INT_MIN, -10001, -1, 0, 1, 10000, INT_MAX};
    for (size_t i = 0; i < sizeof(formats)/sizeof(*formats); i++)
        for (size_t j = 0; j < sizeof(values)/sizeof(*values); j++) {
            char want[128], got[128];
            int a = snprintf(want, sizeof(want), formats[i], values[j]);
            int b = jsport_snprintf(got, sizeof(got), formats[i], values[j]);
            check(a == b && !strcmp(want, got), "integer formatting versus host");
        }
    for (size_t cap = 0; cap < 40; cap++) {
        char want[64] = {0}, got[64] = {0};
        int a = snprintf(want, cap, "%+07d|%.*s|%0*d|%08x", 10000, 3, "January", 5, -20, 0xabc);
        int b = jsport_snprintf(got, cap, "%+07d|%.*s|%0*d|%08x", 10000, 3, "January", 5, -20, 0xabc);
        check(a == b && !memcmp(want, got, sizeof(got)), "format truncation and termination");
    }
    char buf[256], want[256];
    int a = snprintf(want, sizeof(want), "%ld|%llu|%zu|%#x|%#.0o|%hhd", LONG_MIN, ULLONG_MAX, (size_t)SIZE_MAX, 255u, 0u, 255);
    int b = jsport_snprintf(buf, sizeof(buf), "%ld|%llu|%zu|%#x|%#.0o|%hhd", LONG_MIN, ULLONG_MAX, (size_t)SIZE_MAX, 255u, 0u, 255);
    check(a == b && !strcmp(buf, want), "length modifiers and alternate bases");
    jsport_snprintf(buf, sizeof(buf), "%.1f|%.1f|%.1f|%.1f", 1.25, -0.0, 1.0/0.0, 0.0/0.0);
    check(!strcmp(buf, "1.2|-0.0|inf|nan") || !strcmp(buf, "1.2|-0.0|inf|-nan"), "diagnostic ties-even conversion");
    write_limit = 7; output_len = 0;
    check(jsport_fprintf(&jsport_stdout, "%0300d", 42) == 300 && output_len == 300 && output[298] == '4' && output[299] == '2', "stream handles short writes");
    write_limit = 0;
    check(jsport_fprintf(&jsport_stdout, "%s", "test") == -1, "zero write refuses stream output");
    write_limit = SIZE_MAX; write_failed = 1;
    check(jsport_fwrite("abc", 1, 3, &jsport_stdout) == 0, "fwrite failure");
    write_failed = 0;
    check(jsport_fwrite("abc", SIZE_MAX, 3, &jsport_stdout) == 0, "fwrite overflow");
}
static int evaluate(JSContext *ctx, const char *source)
{
    JSValue value = JS_Eval(ctx, source, strlen(source), "target-adapter", JS_EVAL_TYPE_GLOBAL);
    int ok = !JS_IsException(value) && JS_ToBool(ctx, value) == 1;
    if (JS_IsException(value)) {
        JSValue error = JS_GetException(ctx);
        const char *text = JS_ToCString(ctx, error);
        fprintf(stderr, "engine exception: %s\n", text ? text : "unavailable");
        JS_FreeCString(ctx, text); JS_FreeValue(ctx, error);
    }
    JS_FreeValue(ctx, value);
    return ok;
}
static void clocks_and_engine(void)
{
    struct jsport_timeval tv;
    jsport_gettimeofday(&tv, NULL);
    check(tv.tv_sec == 1700000000 && tv.tv_usec == 370000, "wall-clock phase conversion");
    unsetenv("TZ");
    check(jsport_timezone_offset(-1) == -330, "configured timezone offset sign");
    setenv("TZ", "EST5EDT", 1);
    check(jsport_timezone_offset(1704067200000LL) == 300, "winter DST rules");
    check(jsport_timezone_offset(1719792000000LL) == 240, "summer DST rules");
    setenv("TZ", "ABC0DEF,M1.1.4/0,M7.1.4/0", 1);
    check(jsport_timezone_offset(-1) == 0 && jsport_timezone_offset(0) == -60, "negative millisecond floors across transition");
    setenv("TZ", "UTC0", 1);
    for (int i = 0; i < 40; i++) {
        JSRuntime *rt = JS_NewRuntime();
        check(rt != NULL, "runtime constructed with target allocator");
        if (!rt) break;
        JS_SetMemoryLimit(rt, 16 * 1024 * 1024); JS_SetMaxStackSize(rt, 256 * 1024);
        JSContext *ctx = JS_NewContext(rt);
        check(ctx != NULL, "context with target clock");
        if (ctx) {
            check(evaluate(ctx, "Date.now() === 1700000000370 && new Date(-1).toISOString() === '1969-12-31T23:59:59.999Z' && new Date('10000-01-01T00:00:00Z').toString() === 'Invalid Date' && new Date(253402300800000).toISOString() === '+010000-01-01T00:00:00.000Z' && new Date(-62198755200000).toISOString() === '-000001-01-01T00:00:00.000Z'"), "Date epoch, negative and expanded years");
            check(evaluate(ctx, "(1.25).toFixed(1) === '1.3' && Math.sqrt(81) === 9 && (2n ** 100n).toString() === '1267650600228229401496703205376' && /[a-z]+/u.test('yonder') && JSON.parse('{\"ok\":true}').ok && typeof Atomics === 'undefined'"), "adapted language and compiler helper execution");
            check(evaluate(ctx, "try { (function recurse(){recurse()})(); false } catch(e) { e.message === 'stack overflow' }"), "target stack checks remain active");
            check(evaluate(ctx, "globalThis.answer=0; Promise.resolve(21).then(x=>answer=x*2); true"), "Promise enqueue");
            JSContext *job_context;
            check(JS_ExecutePendingJob(rt, &job_context) == 1 && evaluate(ctx, "answer === 42"), "Promise completion");
            JS_FreeContext(ctx);
        }
        JS_FreeRuntime(rt);
        check(live == 0, "repeated lifecycle returns allocator to zero");
    }
    size_t baseline = attempts;
    JSRuntime *rt = JS_NewRuntime();
    size_t constructor_allocations = attempts - baseline;
    JS_FreeRuntime(rt);
    for (size_t i = 1; i <= constructor_allocations; i++) {
        fail_at = attempts + i;
        rt = JS_NewRuntime();
        if (rt) JS_FreeRuntime(rt);
        fail_at = 0;
        check(live == 0, "runtime constructor allocation failure cleanup");
    }
}
int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--zero-allocation")) {
        JSMallocState state = {.malloc_limit = SIZE_MAX};
        (void)jsport_malloc_functions.js_malloc(&state, 0); return 1;
    }
    if (argc == 2 && !strcmp(argv[1], "--clock-failure")) {
        struct jsport_timeval tv;
        js_test_clock_failed = 1; jsport_gettimeofday(&tv, NULL); return 1;
    }
    if (argc == 2 && !strcmp(argv[1], "--leak")) {
        JSRuntime *rt = JS_NewRuntime(); JSContext *ctx = JS_NewContext(rt);
        (void)JS_NewObject(ctx); JS_FreeContext(ctx); JS_FreeRuntime(rt); return 1;
    }
    allocators(); formatting(); clocks_and_engine();
    printf("QuickJS os64 adaptation: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
