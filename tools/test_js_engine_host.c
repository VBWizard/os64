#include <stdio.h>
#include <string.h>
#include "quickjs.h"

static int check(JSContext *context, const char *source)
{
    JSValue value = JS_Eval(context, source, strlen(source), "engine-probe", JS_EVAL_TYPE_GLOBAL);
    int failed = JS_IsException(value) || JS_ToBool(context, value) != 1;
    if (JS_IsException(value)) {
        JSValue exception = JS_GetException(context);
        const char *message = JS_ToCString(context, exception);
        fprintf(stderr, "engine probe: %s\n", message ? message : "exception unavailable");
        JS_FreeCString(context, message);
        JS_FreeValue(context, exception);
    }
    JS_FreeValue(context, value);
    return failed;
}

int main(void)
{
    JSRuntime *runtime = JS_NewRuntime();
    if (!runtime) return 1;
    JS_SetMemoryLimit(runtime, 16 * 1024 * 1024);
    JS_SetMaxStackSize(runtime, 256 * 1024);
    JSContext *context = JS_NewContext(runtime);
    if (!context) { JS_FreeRuntime(runtime); return 1; }
    int failed = check(context,
        "Math.sqrt(81) === 9 && (2n ** 100n).toString() === '1267650600228229401496703205376'"
        " && /[a-z]+/u.test('yonder') && JSON.parse('{\"ok\":true}').ok"
        " && typeof Atomics === 'undefined' && typeof std === 'undefined' && typeof os === 'undefined'");
    failed += check(context, "globalThis.answer=0; Promise.resolve(21).then(x=>answer=x*2); true");
    int jobs = 0, result;
    JSContext *job_context;
    while ((result = JS_ExecutePendingJob(runtime, &job_context)) > 0) jobs++;
    failed += result < 0 || jobs != 1;
    if (result < 0) {
        JSValue exception = JS_GetException(job_context);
        JS_FreeValue(job_context, exception);
    }
    failed += check(context, "answer === 42");
    failed += check(context,
        "try { (function recurse(){recurse()})(); false } catch(e) { e instanceof InternalError && e.message === 'stack overflow' }");
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    printf("QuickJS engine-only host probe: %s (%d failures)\n", failed ? "FAIL" : "PASS", failed);
    return failed != 0;
}
