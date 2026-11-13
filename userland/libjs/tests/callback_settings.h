#ifndef OS64_JS_CALLBACK_SETTINGS_CASES_H
#define OS64_JS_CALLBACK_SETTINGS_CASES_H
#include "os64/js_callbacks.h"

typedef struct { os64_js_callback_t callback; } CallbackHolder;
typedef struct {
    os64_js_runtime_t *runtime;
    os64_js_realm_t *realm[3];
    JSContext *ctx[3];
    os64_js_callback_t *destination, *clearing;
    unsigned bodies, finalized;
    bool finalizer_guard;
} CallbackFixture;
static CallbackFixture *callback_fixture;
static JSClassID callback_holder_slot;

static int callback_realm_number(os64_js_realm_t *realm)
{
    for (unsigned i = 0; i < 3; i++) if (realm == callback_fixture->realm[i]) return (int)i + 1;
    return 0;
}
static JSValue callback_probe(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self; (void)argc; (void)argv;
    os64_js_outcome_t out;
    int entry = callback_realm_number(os64_js_settings_realm(callback_fixture->runtime, OS64_JS_ABI_ID,
                                     OS64_JS_SETTINGS_ENTRY, &out));
    int incumbent = callback_realm_number(os64_js_settings_realm(callback_fixture->runtime, OS64_JS_ABI_ID,
                                         OS64_JS_SETTINGS_INCUMBENT, &out));
    return JS_NewInt32(ctx, 10 * entry + incumbent);
}
static JSValue callback_remember(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)ctx; (void)self;
    if (!argc) return JS_UNDEFINED;
    os64_js_outcome_t out;
    check(os64_js_callback_capture(callback_fixture->runtime, OS64_JS_ABI_ID, argv[0],
          callback_fixture->destination, &out) == OS64_JS_OK, "callback captures conversion-time incumbent");
    return JS_UNDEFINED;
}
static JSValue callback_source(JSContext *ctx, void *opaque)
{
    const char *source = opaque;
    return JS_Eval(ctx, source, os64_strlen(source), "callback-settings.js", JS_EVAL_TYPE_GLOBAL);
}
static JSValue callback_invoke(JSContext *ctx, JSValueConst callback, void *opaque)
{
    (void)opaque;
    os64_js_outcome_t out;
    callback_fixture->bodies++;
    check(os64_js_check_budget(callback_fixture->runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK,
          "callback adapter retains active native guard and task budget");
    return JS_Call(ctx, callback, JS_UNDEFINED, 0, NULL);
}
static JSValue callback_object_invoke(JSContext *ctx, JSValueConst callback, void *opaque)
{
    (void)opaque;
    JSValue method = JS_GetPropertyStr(ctx, callback, "handleEvent");
    if (JS_IsException(method)) return method;
    JSValue result = JS_Call(ctx, method, callback, 0, NULL);
    JS_FreeValue(ctx, method);
    return result;
}
static JSValue callback_capture_value(JSContext *ctx, void *opaque)
{
    (void)ctx;
    os64_js_outcome_t out;
    check(os64_js_callback_capture(callback_fixture->runtime, OS64_JS_ABI_ID, *(JSValue *)opaque,
          callback_fixture->destination, &out) == OS64_JS_OK, "native conversion captures backup without a script frame");
    return JS_UNDEFINED;
}
static void callback_capture_at(unsigned index, JSValue value, os64_js_callback_t *record)
{
    os64_js_outcome_t out;
    JSValue result = JS_UNDEFINED;
    callback_fixture->destination = record;
    check(os64_js_realm_with_settings(callback_fixture->realm[index], callback_fixture->realm[index], OS64_JS_ABI_ID,
          callback_capture_value, &value, &result, &out) == OS64_JS_OK, "callback conversion scope completes");
    JS_FreeValue(callback_fixture->ctx[index], result);
}
static void callback_expect(os64_js_callback_t *record, os64_js_callback_body_t *body, int expected, const char *label)
{
    os64_js_outcome_t out;
    JSValue result = JS_UNDEFINED;
    int32_t number = 0;
    check(os64_js_callback_with_settings(callback_fixture->realm[1], OS64_JS_ABI_ID, record,
          body, NULL, &result, &out) == OS64_JS_OK &&
          JS_ToInt32(callback_fixture->ctx[1], &number, result) == 0 && number == expected, label);
    if (JS_IsException(result)) JS_FreeValue(callback_fixture->ctx[1], JS_GetException(callback_fixture->ctx[1]));
    JS_FreeValue(callback_fixture->ctx[1], result);
}
static JSValue callback_reuse(JSContext *ctx, JSValueConst callback, void *opaque)
{
    os64_js_outcome_t out;
    os64_js_callback_t *record = opaque;
    check(os64_js_callback_clear(callback_fixture->runtime, OS64_JS_ABI_ID, record, &out) == OS64_JS_OK,
          "active callback record cleared for replacement");
    callback_capture_at(0, callback, record);
    return JS_Call(ctx, callback, JS_UNDEFINED, 0, NULL);
}
static JSValue callback_aliases(JSContext *ctx, void *opaque)
{
    os64_js_outcome_t *outer = opaque;
    os64_js_callback_t empty = OS64_JS_CALLBACK_INIT;
    JSValue function = realm_property(callback_fixture->ctx[1], "bound"), result = JS_NewInt32(ctx, 123);
    check(os64_js_callback_capture(callback_fixture->runtime, OS64_JS_ABI_ID, function, &empty, outer) == OS64_JS_BUSY &&
          !empty.runtime, "capture refuses active outcome alias without populating record");
    check(os64_js_callback_clear(callback_fixture->runtime, OS64_JS_ABI_ID, callback_fixture->destination, outer) == OS64_JS_BUSY &&
          callback_fixture->destination->runtime == callback_fixture->runtime, "clear refuses active outcome alias without dropping reference");
    check(os64_js_callback_with_settings(callback_fixture->realm[1], OS64_JS_ABI_ID, callback_fixture->destination,
          callback_invoke, NULL, &result, outer) == OS64_JS_BUSY && JS_VALUE_GET_INT(result) == 123,
          "callback invocation refuses active outcome alias before body or result mutation");
    os64_js_outcome_t local;
    check(os64_js_callback_capture(callback_fixture->runtime, OS64_JS_ABI_ID, JS_NULL, &empty, &local) == OS64_JS_BAD_ARGUMENT &&
          !empty.runtime, "callback capture rejects a non-object transactionally");
    JS_FreeValue(ctx, function);
    return JS_UNDEFINED;
}
static void callback_holder_mark(JSRuntime *engine, JSValueConst object, JS_MarkFunc *mark)
{
    CallbackHolder *holder = JS_GetOpaque(object, callback_holder_slot);
    if (holder) os64_js_callback_mark(engine, &holder->callback, mark);
}
static void callback_holder_free(JSRuntime *engine, JSValue object)
{
    CallbackHolder *holder = JS_GetOpaque(object, callback_holder_slot);
    if (!holder) return;
    os64_js_outcome_t out;
    if (callback_fixture->finalizer_guard) {
        check(os64_js_check_budget(callback_fixture->runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK,
              "last invocation hold releases inside the native settings guard");
        check(callback_fixture->clearing && !callback_fixture->clearing->runtime &&
              JS_IsUndefined(callback_fixture->clearing->value), "record is empty before callback finalization can reenter");
        check(os64_js_callback_clear(callback_fixture->runtime, OS64_JS_ABI_ID, callback_fixture->clearing, &out) == OS64_JS_OK,
              "reentrant clear observes an empty record");
        callback_fixture->finalizer_guard = false;
    }
    check(os64_js_callback_clear(callback_fixture->runtime, OS64_JS_ABI_ID, &holder->callback, &out) == OS64_JS_OK,
          "GC holder clears callback during finalization");
    callback_fixture->finalized++;
    js_free_rt(engine, holder);
}
static JSValue callback_data(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic, JSValue *data)
{
    (void)self; (void)argc; (void)argv; (void)magic;
    check(JS_GetOpaque(data[0], callback_holder_slot) != NULL, "active invocation retains callback data object");
    return JS_NewInt32(ctx, 77);
}
static JSValue callback_self_clear(JSContext *ctx, JSValueConst callback, void *opaque)
{
    (void)opaque;
    os64_js_outcome_t out;
    unsigned before = callback_fixture->finalized;
    check(os64_js_callback_clear(callback_fixture->runtime, OS64_JS_ABI_ID, callback_fixture->clearing, &out) == OS64_JS_OK,
          "callback removes its own native registration");
    JS_RunGC(JS_GetRuntime(ctx));
    check(callback_fixture->finalized == before, "invocation snapshot outlives self-removal and GC");
    callback_fixture->finalizer_guard = true;
    return JS_Call(ctx, callback, JS_UNDEFINED, 0, NULL);
}
static JSValue callback_clear_root(JSContext *ctx, void *opaque)
{
    (void)ctx;
    os64_js_outcome_t out;
    callback_fixture->finalizer_guard = true;
    check(os64_js_callback_clear(callback_fixture->runtime, OS64_JS_ABI_ID, opaque, &out) == OS64_JS_OK,
          "native root releases its last callback reference");
    return JS_UNDEFINED;
}
static JSValue callback_make_holder(JSContext *ctx, CallbackHolder **holder)
{
    JSValue object = JS_NewObjectClass(ctx, callback_holder_slot);
    *holder = NULL;
    if (JS_IsException(object)) return object;
    *holder = js_mallocz(ctx, sizeof(**holder));
    if (!*holder) { JS_FreeValue(ctx, object); return JS_EXCEPTION; }
    (*holder)->callback = (os64_js_callback_t)OS64_JS_CALLBACK_INIT;
    JS_SetOpaque(object, *holder);
    return object;
}
static void callback_settings_cases(void)
{
    for (unsigned policy = 0; policy < 2; policy++) {
        os64_js_config_t config = fixture_config();
        os64_js_runtime_t *runtime = NULL, *other = NULL;
        os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config, policy ? OS64_JS_TEARDOWN_RECLAIM : OS64_JS_TEARDOWN_FATAL,
              OS64_JS_ABI_ID, &runtime, &out) == OS64_JS_OK, "callback record runtime created");
        if (!runtime) continue;
        CallbackFixture fixture = {.runtime = runtime}; callback_fixture = &fixture;
        fixture.realm[0] = os64_js_primary_realm(runtime, OS64_JS_ABI_ID, &out);
        check(os64_js_realm_create(runtime, OS64_JS_ABI_ID, &fixture.realm[1], &out) == OS64_JS_OK &&
              os64_js_realm_create(runtime, OS64_JS_ABI_ID, &fixture.realm[2], &out) == OS64_JS_OK, "callback fixture has three realms");
        if (!fixture.realm[1] || !fixture.realm[2]) { os64_js_destroy(runtime); continue; }
        for (unsigned i = 0; i < 3; i++) fixture.ctx[i] = os64_js_realm_context(fixture.realm[i], OS64_JS_ABI_ID, &out);
        JSContext *a = fixture.ctx[0], *b = fixture.ctx[1], *c = fixture.ctx[2];
        JSRuntime *engine = JS_GetRuntime(a);
        for (unsigned i = 0; i < 3; i++) {
            JSValue global = JS_GetGlobalObject(fixture.ctx[i]);
            check(JS_SetPropertyStr(fixture.ctx[i], global, "settings", JS_NewCFunction(b, callback_probe, "settings", 0)) == 1 &&
                  JS_SetPropertyStr(fixture.ctx[i], global, "remember", JS_NewCFunction(a, callback_remember, "remember", 1)) == 1,
                  "callback capture binding and callback defining realms are independent");
            JS_FreeValue(fixture.ctx[i], global);
        }
        check(realm_execute(fixture.realm[1], "globalThis.bound=settings.bind(null);globalThis.author=()=>settings();"
              "globalThis.abrupt=()=>{throw 42};globalThis.object={}", &out) == OS64_JS_OK, "callback reference fixtures created");
        realm_share(b, c, "bound"); realm_share(b, c, "author");
        check(realm_execute(fixture.realm[2], "globalThis.lookup=0;globalThis.getter=()=>{lookup=settings();return settings};"
              "globalThis.register=()=>remember(bound)", &out) == OS64_JS_OK, "foreign callback conversion and getter fixtures created");
        realm_share(c, a, "register");
        check(os64_js_create(&config, OS64_JS_ABI_ID, &other, &out) == OS64_JS_OK, "callback foreign runtime created");
        os64_js_callback_t native = OS64_JS_CALLBACK_INIT, author = OS64_JS_CALLBACK_INIT,
            object_record = OS64_JS_CALLBACK_INIT, abrupt = OS64_JS_CALLBACK_INIT, owned = OS64_JS_CALLBACK_INIT;
        JSValue function = realm_property(b, "bound"), result = JS_UNDEFINED;
        check(os64_js_callback_capture(runtime, OS64_JS_ABI_ID, function, &native, &out) == OS64_JS_ADMISSION_REFUSED &&
              !native.runtime && JS_IsUndefined(native.value), "callback capture refuses missing checked incumbent");
        check(os64_js_task_begin(runtime, OS64_JS_ABI_ID, "callback-record", &out) == OS64_JS_OK, "callback fixture task opened");
        fixture.destination = &native;
        check(os64_js_realm_with_settings(fixture.realm[0], NULL, OS64_JS_ABI_ID, callback_source,
              "register()", &result, &out) == OS64_JS_OK && native.incumbent == fixture.realm[2],
              "registration script C captures C despite native binding A and callback B");
        JS_FreeValue(c, result);
        callback_capture_at(0, function, &author);
        callback_expect(&native, callback_invoke, 23, "native callback retains registration C across host delivery");
        callback_expect(&author, callback_invoke, 21, "same callback independently captures registration A");
        fixture.destination = &native;
        check(os64_js_realm_with_settings(fixture.realm[0], NULL, OS64_JS_ABI_ID, callback_aliases, &out, &result, &out) == OS64_JS_OK,
              "callback refusals leave outer outcome intact");
        JS_FreeValue(a, result);
        check(os64_js_callback_with_settings(fixture.realm[1], OS64_JS_ABI_ID, &native, callback_reuse, &native, &result, &out) == OS64_JS_OK &&
              JS_IsNumber(result) && JS_VALUE_GET_INT(result) == 23 && native.incumbent == fixture.realm[0],
              "record reuse changes future capture while active invocation retains incumbent C");
        JS_FreeValue(b, result);
        callback_expect(&native, callback_invoke, 21, "replacement registration delivers captured A");
        check(os64_js_callback_clear(runtime, OS64_JS_ABI_ID, &native, &out) == OS64_JS_OK, "replacement record cleared");
        callback_capture_at(2, function, &native);
        size_t before = os64_js_heap_bytes(runtime);
        void *saved_value = JS_VALUE_GET_PTR(native.value);
        os64_js_realm_t *saved_incumbent = native.incumbent;
        JS_Throw(a, JS_NewInt32(a, 41));
        check(os64_js_callback_capture(runtime, OS64_JS_ABI_ID, function, &native, &out) == OS64_JS_BAD_ARGUMENT &&
              native.runtime == runtime && native.incumbent == saved_incumbent && JS_VALUE_GET_PTR(native.value) == saved_value,
              "populated capture refusal preserves prior ownership");
        check(os64_js_callback_clear(runtime, "wrong", &native, &out) == OS64_JS_ABI_MISMATCH &&
              (!other || os64_js_callback_clear(other, OS64_JS_ABI_ID, &native, &out) == OS64_JS_BAD_ARGUMENT) &&
              os64_js_heap_bytes(runtime) == before, "invalid clear leaves held reference and allocation charge unchanged");
        JSValue error = JS_GetException(a);
        check(JS_IsNumber(error) && JS_VALUE_GET_INT(error) == 41, "capture and clear refusals preserve exception"); JS_FreeValue(a, error);
        check(os64_js_callback_clear(runtime, OS64_JS_ABI_ID, &author, &out) == OS64_JS_OK, "callback record can be reused after clear");
        JS_FreeValue(b, function); function = realm_property(b, "author"); callback_capture_at(2, function, &author); JS_FreeValue(b, function);
        callback_expect(&author, callback_invoke, 22, "new author frame takes incumbent precedence over captured C");
        function = realm_property(b, "abrupt"); callback_capture_at(2, function, &abrupt); JS_FreeValue(b, function);
        check(os64_js_callback_with_settings(fixture.realm[1], OS64_JS_ABI_ID, &abrupt, callback_invoke, NULL, &result, &out) == OS64_JS_EXCEPTION &&
              JS_IsException(result), "callback exception remains pending after guarded cleanup");
        error = JS_GetException(b); check(JS_IsNumber(error) && JS_VALUE_GET_INT(error) == 42, "callback exception identity preserved"); JS_FreeValue(b, error);
        callback_expect(&native, callback_invoke, 23, "abrupt callback restores previous settings and guard");

        JSValue object = realm_property(b, "object");
        JSAtom key = JS_NewAtom(b, "handleEvent");
        check(JS_DefinePropertyGetSet(b, object, key, realm_property(c, "getter"), JS_UNDEFINED, JS_PROP_CONFIGURABLE) == 1,
              "object B has a getter from C and native method from B"); JS_FreeAtom(b, key);
        callback_capture_at(0, object, &object_record); JS_FreeValue(b, object);
        callback_expect(&object_record, callback_object_invoke, 21, "object callback adapter retains captured A after foreign getter returns");
        JSValue lookup = realm_property(c, "lookup"); check(JS_VALUE_GET_INT(lookup) == 23, "handleEvent lookup runs inside entry B with getter script C"); JS_FreeValue(c, lookup);

        callback_holder_slot = os64_js_class_id(&callback_holder_slot);
        const JSClassDef definition = {.class_name="CallbackHolder", .finalizer=callback_holder_free, .gc_mark=callback_holder_mark};
        check(JS_NewClass(engine, callback_holder_slot, &definition) == 0, "traceable callback holder class registered");
        CallbackHolder *holder;
        object = callback_make_holder(b, &holder);
        check(holder != NULL, "callback lifetime holder allocated");
        if (holder) {
            function = JS_NewCFunctionData(b, callback_data, 0, 0, 1, &object);
            callback_capture_at(2, function, &owned); JS_FreeValue(b, function); JS_FreeValue(b, object);
            JS_RunGC(engine); check(fixture.finalized == 0, "C-root callback keeps its data holder alive");
            fixture.clearing = &owned;
            callback_expect(&owned, callback_self_clear, 77, "active callback snapshot survives its registration removal");
            check(fixture.finalized == 1 && !fixture.finalizer_guard, "last snapshot releases inside scope and finalizes once");
        }
        object = callback_make_holder(b, &holder);
        check(holder != NULL, "direct cleanup holder allocated");
        if (holder) {
            function = JS_NewCFunctionData(b, callback_data, 0, 0, 1, &object);
            callback_capture_at(2, function, &owned); JS_FreeValue(b, function); JS_FreeValue(b, object);
            check(os64_js_realm_with_settings(fixture.realm[1], NULL, OS64_JS_ABI_ID, callback_clear_root,
                  &owned, &result, &out) == OS64_JS_OK, "direct clear permits reentrant native finalization");
            JS_FreeValue(b, result);
            check(fixture.finalized == 2 && !fixture.finalizer_guard, "record reset precedes last-reference finalizer");
        }
        object = callback_make_holder(b, &holder);
        check(holder != NULL, "cycle callback holder allocated");
        if (holder) {
            function = JS_NewCFunctionData(b, callback_data, 0, 0, 1, &object);
            callback_capture_at(2, function, &holder->callback); JS_FreeValue(b, function); JS_FreeValue(b, object);
            JS_RunGC(engine);
            check(fixture.finalized == 3, "traced callback-holder cycle is collectible");
        }
        object = callback_make_holder(b, &holder);
        check(holder != NULL, "runtime teardown callback holder allocated");
        if (holder) {
            function = JS_NewCFunctionData(b, callback_data, 0, 0, 1, &object);
            callback_capture_at(2, function, &holder->callback); JS_FreeValue(b, function); JS_FreeValue(b, object);
        }
        unsigned calls = fixture.bodies;
        check(os64_js_realm_retire(fixture.realm[2], OS64_JS_ABI_ID, &out) == OS64_JS_OK, "captured incumbent retired");
        result = JS_NewInt32(b, 123);
        check(os64_js_callback_with_settings(fixture.realm[1], OS64_JS_ABI_ID, &native, callback_invoke, NULL, &result, &out) == OS64_JS_REALM_RETIRED &&
              fixture.bodies == calls && JS_VALUE_GET_INT(result) == 123, "retired capture refuses delivery before body or result mutation");
        os64_js_cancel(runtime);
        check(os64_js_callback_with_settings(fixture.realm[1], OS64_JS_ABI_ID, &object_record, callback_object_invoke, NULL, &result, &out) == OS64_JS_FAILED_RUNTIME,
              "failed runtime refuses callback delivery");
        os64_js_callback_t *records[] = {&native, &author, &object_record, &abrupt, &owned};
        for (unsigned i = 0; i < sizeof(records)/sizeof(records[0]); i++)
            check(os64_js_callback_clear(runtime, OS64_JS_ABI_ID, records[i], &out) == OS64_JS_OK &&
                  os64_js_callback_clear(runtime, OS64_JS_ABI_ID, records[i], &out) == OS64_JS_OK && !records[i]->runtime,
                  "callback cleanup is idempotent after retirement and cancellation");
        check(os64_js_task_end(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_CANCELLED, "callback cleanup does not clear task cancellation");
        if (other) os64_js_destroy(other);
        os64_js_destroy(runtime);
        check(fixture.finalized == 4, "runtime destruction finalizes the remaining callback cycle after failure");
        callback_fixture = NULL;
    }
}
#endif
