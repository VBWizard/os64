#include "os64/js_engine.h"
#include "quickjs.h"

static JSValue twice(JSContext *ctx, JSValueConst self, int argc,
                     JSValueConst *argv)
{
    (void)self;
    if (argc != 1)
        return JS_ThrowTypeError(ctx, "twice expects one argument");
    int32_t value;
    if (JS_ToInt32(ctx, &value, argv[0]) < 0) return JS_EXCEPTION;
    return JS_NewFloat64(ctx, 2.0 * value);
}

int js_example_install_twice(os64_js_runtime_t *runtime)
{
    JSContext *ctx = os64_js_context(runtime);
    if (!ctx) return -1;
    JSValue global = JS_GetGlobalObject(ctx);
    if (JS_IsException(global)) return -1;
    JSValue fn = JS_NewCFunction(ctx, twice, "twice", 1);
    if (JS_IsException(fn)) { JS_FreeValue(ctx, global); return -1; }
    /* SetPropertyStr consumes fn even on failure; global remains ours. */
    int result = JS_SetPropertyStr(ctx, global, "twice", fn);
    JS_FreeValue(ctx, global);
    return result < 0 ? -1 : 0;
}
