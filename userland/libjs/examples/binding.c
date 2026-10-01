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

int js_example_install_twice(os64_js_runtime_t *runtime, os64_js_outcome_t *outcome)
{
    JSContext *ctx = os64_js_context(runtime, OS64_JS_ABI_ID, outcome);
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

int js_example_register_class(os64_js_runtime_t *runtime, os64_js_outcome_t *outcome)
{
    static JSClassID class_id;
    static const JSClassDef definition = {.class_name = "Example"};
    JSContext *ctx = os64_js_context(runtime, OS64_JS_ABI_ID, outcome);
    if (!ctx) return -1;
    /* The wrapper serializes access to this slot across independent runtimes. */
    JSClassID id = os64_js_class_id(&class_id);
    if (!id) return -1;
    JSRuntime *engine = JS_GetRuntime(ctx);
    if (JS_IsRegisteredClass(engine, id)) return 0;
    return JS_NewClass(engine, id, &definition);
}
