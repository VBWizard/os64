#include "internal.h"

/* THE STREAMS STANDARD (STREAMS.md): the C half. The standard itself is
 * streams.js, self-hosted JavaScript, carried in this object as bytes
 * (the .incbin below) and run in each realm by the Modern Web's streams
 * row. This file does three things:
 *
 *   - it captures the INTRINSICS streams.js runs on, before page script,
 *     and supplies private callback and continuation helpers in the same
 *     null-prototype object; streams.js refuses to load without any
 *     one of them, so this list and its own cannot drift apart unseen;
 *   - it defines the interfaces streams.js returns on the global, as
 *     WebIDL defines an interface object (writable, configurable, not
 *     enumerable);
 *   - it keeps the DOOR streams.js returns beside them, and answers
 *     libdom's d_stream_* verbs through it (internal.h).
 *
 * The script's frames carry the name OS64_DOM_STREAMS_SCRIPT (dom.h), which
 * is how a host tells a throw inside it from the page's own. */

#ifndef LIBDOM_STREAMS_JS
#define LIBDOM_STREAMS_JS "libdom/streams.js"
#endif

/* The script, NUL-terminated as JS_Eval requires. The path is the build's:
 * the userland build runs from userland/, a host build passes its own. */
__asm__(".section .rodata\n"
        ".global d_streams_source\n"
        ".hidden d_streams_source\n"
        "d_streams_source:\n"
        ".incbin \"" LIBDOM_STREAMS_JS "\"\n"
        ".global d_streams_source_end\n"
        ".hidden d_streams_source_end\n"
        "d_streams_source_end:\n"
        ".byte 0\n"
        ".previous\n");
extern const char d_streams_source[], d_streams_source_end[];

static const char kScriptName[] = OS64_DOM_STREAMS_SCRIPT;

/* THE COMPILED SCRIPT, kept for the process. Compiling streams.js costs a
 * realm about 5 ms on a desktop core; reading its bytecode back costs a
 * thirtieth of that (STREAMS.md § 8), so the first realm compiles and
 * keeps the bytecode here, and every later one reads it. The bytes never
 * leave the process, so the engine that reads them is the one that wrote
 * them. The buffer is static rather than allocated, so the cache is no
 * allocation of anybody's to account for; a script that outgrows it is
 * compiled by every realm, which is slower and nothing else. The state
 * word publishes the bytes: one realm claims FILLING, fills, then
 * releases READY, and a realm reads them only after acquiring READY. */
#define D_STREAMS_BYTECODE_MAX ((size_t)128 * 1024)
enum { CACHE_EMPTY, CACHE_FILLING, CACHE_READY, CACHE_UNCACHED };
static uint8_t s_bytecode[D_STREAMS_BYTECODE_MAX];
static size_t s_bytecode_length;
static uint32_t s_bytecode_state;

/* The script's completion value, the factory, or JS_EXCEPTION. A compile
 * keeps no source text: an interface's toString then reads as a native
 * one does, and a realm holds a third less (STREAMS.md § 8). File names
 * and line numbers stay, which is what a failure's frames need. */
static JSValue streams_factory(JSContext *ctx)
{
    if (__atomic_load_n(&s_bytecode_state, __ATOMIC_ACQUIRE) == CACHE_READY) {
        JSValue function = JS_ReadObject(ctx, s_bytecode, s_bytecode_length, JS_READ_OBJ_BYTECODE);
        if (JS_IsException(function)) return function;
        return JS_EvalFunction(ctx, function);
    }
    JSRuntime *rt = JS_GetRuntime(ctx);
    int strip = JS_GetStripInfo(rt);
    JS_SetStripInfo(rt, strip | JS_STRIP_SOURCE);
    JSValue function = JS_Eval(ctx, d_streams_source, (size_t)(d_streams_source_end - d_streams_source), kScriptName,
                               JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    JS_SetStripInfo(rt, strip);
    if (JS_IsException(function)) return function;
    uint32_t expected = CACHE_EMPTY;
    if (__atomic_compare_exchange_n(&s_bytecode_state, &expected, CACHE_FILLING, false, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED)) {
        size_t length = 0;
        uint8_t *bytes = JS_WriteObject(ctx, &length, function, JS_WRITE_OBJ_BYTECODE);
        uint32_t state = CACHE_UNCACHED;
        if (bytes != NULL && length <= sizeof(s_bytecode)) {
            os64_memcpy(s_bytecode, bytes, length);
            s_bytecode_length = length;
            state = CACHE_READY;
        }
        js_free(ctx, bytes);
        /* A write the engine had no memory for is no verdict on the
         * script: the next realm tries again. Its exception is cleared
         * only when catchable; one that is not (the runtime stopping)
         * stays pending, and this realm's streams are refused with it. */
        bool stopped = false;
        if (bytes == NULL) {
            state = CACHE_EMPTY;
            if (d_exception_catchable(ctx)) JS_FreeValue(ctx, JS_GetException(ctx));
            else stopped = true;
        }
        __atomic_store_n(&s_bytecode_state, state, __ATOMIC_RELEASE);
        if (stopped) {
            JS_FreeValue(ctx, function);
            return JS_EXCEPTION;
        }
    }
    return JS_EvalFunction(ctx, function);
}

size_t d_streams_bytecode_kept(void)
{
    return __atomic_load_n(&s_bytecode_state, __ATOMIC_ACQUIRE) == CACHE_READY ? s_bytecode_length : 0;
}

/* An intrinsic: the name streams.js knows it by, and where it is, as a
 * dotted path from the global. GETTER takes the path's last member's own
 * getter instead of its value. */
enum { VALUE, GETTER };

static const struct {
    const char *name, *path;
    int take;
} kIntrinsics[] = {
    {"Promise", "Promise", VALUE},
    {"Promise_then", "Promise.prototype.then", VALUE},
    {"Promise_reject", "Promise.reject", VALUE},
    {"Reflect_apply", "Reflect.apply", VALUE},
    {"Object_defineProperty", "Object.defineProperty", VALUE},
    {"Object_getPrototypeOf", "Object.getPrototypeOf", VALUE},
    {"Object_getOwnPropertyNames", "Object.getOwnPropertyNames", VALUE},
    {"Object_getOwnPropertyDescriptor", "Object.getOwnPropertyDescriptor", VALUE},
    {"Object_create", "Object.create", VALUE},
    {"TypeError", "TypeError", VALUE},
    {"RangeError", "RangeError", VALUE},
    {"Uint8Array", "Uint8Array", VALUE},
    {"Uint8Array_set", "Uint8Array.prototype.set", VALUE},
    /* A byte stream's buffers and views: streams.js takes the getters and
     * transfer() it needs from ArrayBuffer's and DataView's prototypes,
     * and builds the view a BYOB read is answered with from these
     * constructors. */
    {"ArrayBuffer", "ArrayBuffer", VALUE},
    {"DataView", "DataView", VALUE},
    {"Int8Array", "Int8Array", VALUE},
    {"Uint8ClampedArray", "Uint8ClampedArray", VALUE},
    {"Int16Array", "Int16Array", VALUE},
    {"Uint16Array", "Uint16Array", VALUE},
    {"Int32Array", "Int32Array", VALUE},
    {"Uint32Array", "Uint32Array", VALUE},
    {"Float16Array", "Float16Array", VALUE},
    {"Float32Array", "Float32Array", VALUE},
    {"Float64Array", "Float64Array", VALUE},
    {"BigInt64Array", "BigInt64Array", VALUE},
    {"BigUint64Array", "BigUint64Array", VALUE},
    {"structuredClone", "structuredClone", VALUE},
    {"Symbol_asyncIterator", "Symbol.asyncIterator", VALUE},
    {"Symbol_iterator", "Symbol.iterator", VALUE},
    {"Symbol_toStringTag", "Symbol.toStringTag", VALUE},
    {"AbortController", "AbortController", VALUE},
    {"AbortController_abort", "AbortController.prototype.abort", VALUE},
    {"AbortController_signal", "AbortController.prototype.signal", GETTER},
    {"AbortSignal_aborted", "AbortSignal.prototype.aborted", GETTER},
    {"AbortSignal_reason", "AbortSignal.prototype.reason", GETTER},
    /* The Encoding standard's streams decode and encode through the
     * realm's TextDecoder and TextEncoder, and look at a string's code
     * units through String's own methods. */
    {"TextDecoder", "TextDecoder", VALUE},
    {"TextDecoder_decode", "TextDecoder.prototype.decode", VALUE},
    {"TextDecoder_encoding", "TextDecoder.prototype.encoding", GETTER},
    {"TextDecoder_fatal", "TextDecoder.prototype.fatal", GETTER},
    {"TextDecoder_ignoreBOM", "TextDecoder.prototype.ignoreBOM", GETTER},
    {"TextEncoder", "TextEncoder", VALUE},
    {"TextEncoder_encode", "TextEncoder.prototype.encode", VALUE},
    {"String_charCodeAt", "String.prototype.charCodeAt", VALUE},
    {"String_slice", "String.prototype.slice", VALUE},
};

/* The value at `path` from `global` (taken), JS_UNDEFINED when a step is
 * missing, JS_EXCEPTION on an exception. Each step is a plain get, the
 * prototype chain included: at a realm's start nothing a page wrote is
 * there to be found. */
static JSValue intrinsic_at(JSContext *ctx, JSValueConst global, const char *path, int take)
{
    JSValue at = JS_DupValue(ctx, global);
    char step[48];
    for (const char *p = path; *p != '\0';) {
        size_t n = 0;
        while (p[n] != '\0' && p[n] != '.' && n + 1 < sizeof(step)) n++;
        os64_memcpy(step, p, n);
        step[n] = '\0';
        p += n;
        if (*p == '.') p++;
        if (!JS_IsObject(at)) {
            JS_FreeValue(ctx, at);
            return JS_UNDEFINED;
        }
        if (*p == '\0' && take == GETTER) {
            JSAtom atom = JS_NewAtom(ctx, step);
            JSPropertyDescriptor descriptor;
            int found = atom == JS_ATOM_NULL ? -1 : JS_GetOwnProperty(ctx, &descriptor, at, atom);
            if (atom != JS_ATOM_NULL) JS_FreeAtom(ctx, atom);
            JS_FreeValue(ctx, at);
            if (found < 0) return JS_EXCEPTION;
            if (found == 0) return JS_UNDEFINED;
            JS_FreeValue(ctx, descriptor.value);
            JS_FreeValue(ctx, descriptor.setter);
            return descriptor.getter;
        }
        JSValue next = JS_GetPropertyStr(ctx, at, step);
        JS_FreeValue(ctx, at);
        if (JS_IsException(next)) return next;
        at = next;
    }
    return at;
}

/* %AsyncIteratorPrototype%, which no global names: the prototype of the
 * prototype of an async generator function's `prototype` object, walked
 * with the engine's own [[GetPrototypeOf]]. */
static JSValue async_iterator_prototype(JSContext *ctx)
{
    static const char kGenerator[] = "(async function* () {})";
    JSValue generator = JS_Eval(ctx, kGenerator, sizeof(kGenerator) - 1, kScriptName, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(generator)) return generator;
    JSValue instance_prototype = JS_GetPropertyStr(ctx, generator, "prototype");
    JS_FreeValue(ctx, generator);
    if (JS_IsException(instance_prototype)) return instance_prototype;
    JSValue generator_prototype = JS_GetPrototype(ctx, instance_prototype);
    JS_FreeValue(ctx, instance_prototype);
    if (JS_IsException(generator_prototype)) return generator_prototype;
    JSValue iterator_prototype = JS_GetPrototype(ctx, generator_prototype);
    JS_FreeValue(ctx, generator_prototype);
    return iterator_prototype;
}

/* The self-hosted algorithms invoke author callbacks through this private
 * guard. Pure stream records are not document activity; the actual callback
 * realm controls invocation, including through a retained constructor. */
static JSValue require_callback(JSContext *ctx, JSValueConst self, int argc,
                                 JSValueConst *argv, int magic, JSValue *data)
{
    (void)self; (void)magic;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "Expected a stream callback");
    return d_require_callback(dom, ctx, argv[0]) ? JS_UNDEFINED : JS_EXCEPTION;
}

/* The hidden doors retain raw private-field probes, never public methods or
 * page-controlled properties. A retained foreign object keeps its genuine
 * brand after departure; probing does not schedule activity in its realm. */
static JSValue foreign_stream_slot(JSContext *ctx, JSValueConst self, int argc,
                                   JSValueConst *argv, int magic, JSValue *data)
{
    (void)self; (void)magic;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (!dom) return JS_EXCEPTION;
    if (argc < 2 || !JS_IsObject(argv[1]) || !dom->cluster) return JS_UNDEFINED;
    for (size_t i = 0; i < dom->cluster->count; ++i) {
        os64_dom_t *member = dom->cluster->members[i];
        if (member == dom || member->closed || !member->streams_door) continue;
        JSValue probe = JS_GetPropertyStr(ctx, member->streams_door->value, "brand");
        if (JS_IsException(probe)) return probe;
        JSValue record = JS_Call(ctx, probe, JS_UNDEFINED, 2, argv);
        JS_FreeValue(ctx, probe);
        if (!JS_IsUndefined(record)) return record;
    }
    return JS_UNDEFINED;
}

/* Only the private factory can supply these functions. Their algorithms keep
 * settled data usable after creator departure; author invocation goes through
 * RequireCallback inside streams.js, rather than inheriting neutrality. */
static JSValue internal_reaction_call(JSContext *ctx, JSValueConst self, int argc,
                                     JSValueConst *argv, int magic, JSValue *data)
{
    (void)self; (void)magic;
    return JS_Call(ctx, data[0], JS_UNDEFINED, argc, argv);
}

static JSValue internal_reaction(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    if (!argc || JS_IsUndefined(argv[0])) return JS_UNDEFINED;
    if (!JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "Expected an internal stream reaction");
    return JS_NewCFunctionDataNeutral(ctx, internal_reaction_call, 1, 0, 1, argv);
}

static JSValue internal_microtask_run(JSContext *ctx, int argc, JSValueConst *argv)
{
    (void)argc;
    return JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);
}

static JSValue internal_microtask(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    if (!argc || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "Expected an internal stream microtask");
    if (JS_EnqueueJobOwned(ctx, NULL, NULL, internal_microtask_run, 1, argv) < 0) return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue internal_abort_add(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    if (argc < 2) return JS_ThrowTypeError(ctx, "Expected a signal and internal abort algorithm");
    return d_signal_add_internal(ctx, argv[0], argv[1]) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

static JSValue internal_abort_remove(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    if (argc >= 2) d_signal_remove_internal(ctx, argv[0], argv[1]);
    return JS_UNDEFINED;
}

/* The intrinsics object and private callback guard, or JS_EXCEPTION. A capture that is missing is
 * left out, and streams.js names it when it refuses to load. */
static JSValue intrinsics(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    JSValue object = JS_NewObjectProto(ctx, JS_NULL);
    if (JS_IsException(object)) return object;
    for (size_t i = 0; i < sizeof(kIntrinsics) / sizeof(kIntrinsics[0]); i++) {
        JSValue value = intrinsic_at(ctx, global, kIntrinsics[i].path, kIntrinsics[i].take);
        if (JS_IsException(value)) goto fail;
        if (JS_IsUndefined(value)) continue;
        if (JS_DefinePropertyValueStr(ctx, object, kIntrinsics[i].name, value, JS_PROP_C_W_E) < 0) goto fail;
    }
    JSValue prototype = async_iterator_prototype(ctx);
    if (JS_IsException(prototype)) goto fail;
    if (JS_DefinePropertyValueStr(ctx, object, "AsyncIteratorPrototype", prototype, JS_PROP_C_W_E) < 0) goto fail;
    if (d_method(dom, ctx, object, "RequireCallback", require_callback, 1, 0) < 0) goto fail;
    if (d_method(dom, ctx, object, "ForeignStreamSlot", foreign_stream_slot, 2, 0) < 0) goto fail;
    static const struct { const char *name; JSCFunction *function; int length; } helpers[] = {
        {"InternalReaction", internal_reaction, 1},
        {"InternalMicrotask", internal_microtask, 1},
        {"AddAbortAlgorithm", internal_abort_add, 2},
        {"RemoveAbortAlgorithm", internal_abort_remove, 2},
    };
    for (size_t i = 0; i < sizeof(helpers) / sizeof(helpers[0]); i++) {
        JSValue function = JS_NewCFunction(ctx, helpers[i].function, helpers[i].name, helpers[i].length);
        if (JS_IsException(function)) goto fail;
        if (JS_DefinePropertyValueStr(ctx, object, helpers[i].name, function, JS_PROP_C_W_E) < 0) goto fail;
    }
    return object;
fail:
    JS_FreeValue(ctx, object);
    return JS_EXCEPTION;
}

/* The interfaces streams.js returns, defined on the global by name; the
 * Modern Web's table lists the same names (modern.c). */
static const char *const kInterfaces[] = {
    "ReadableStream", "ReadableStreamDefaultReader", "ReadableStreamDefaultController",
    "ReadableStreamBYOBReader", "ReadableByteStreamController", "ReadableStreamBYOBRequest",
    "WritableStream", "WritableStreamDefaultWriter", "WritableStreamDefaultController",
    "TransformStream", "TransformStreamDefaultController",
    "ByteLengthQueuingStrategy", "CountQueuingStrategy",
    "TextDecoderStream", "TextEncoderStream",
};

int d_streams_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    JSValue factory = streams_factory(ctx);
    if (JS_IsException(factory)) return -1;
    JSValue captured = intrinsics(dom, ctx, global);
    if (JS_IsException(captured)) {
        JS_FreeValue(ctx, factory);
        return -1;
    }
    JSValue exported = JS_Call(ctx, factory, JS_UNDEFINED, 1, (JSValueConst *)&captured);
    JS_FreeValue(ctx, factory);
    JS_FreeValue(ctx, captured);
    if (JS_IsException(exported)) return -1;
    int result = -1;
    for (size_t i = 0; i < sizeof(kInterfaces) / sizeof(kInterfaces[0]); i++) {
        JSValue interface = JS_GetPropertyStr(ctx, exported, kInterfaces[i]);
        if (JS_IsException(interface)) goto done;
        if (JS_DefinePropertyValueStr(ctx, global, kInterfaces[i], interface,
                                      JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
            goto done;
    }
    dom->streams_door = d_retain(dom, ctx, JS_GetPropertyStr(ctx, exported, "door"));
    if (dom->streams_door != NULL) result = 0;
done:
    JS_FreeValue(ctx, exported);
    return result;
}

/* The door's verb `name` called with `argv`; its result, or JS_EXCEPTION. */
static JSValue door_call(os64_dom_t *dom, JSContext *ctx, const char *name, int argc, JSValueConst *argv)
{
    if (dom->streams_door == NULL) return JS_ThrowTypeError(ctx, "Streams are not supported");
    JSValue verb = JS_GetPropertyStr(ctx, dom->streams_door->value, name);
    if (JS_IsException(verb)) return verb;
    JSValue result = JS_Call(ctx, verb, JS_UNDEFINED, argc, argv);
    JS_FreeValue(ctx, verb);
    return result;
}

static int door_bool(os64_dom_t *dom, JSContext *ctx, const char *name, JSValueConst stream)
{
    JSValue answer = door_call(dom, ctx, name, 1, &stream);
    if (JS_IsException(answer)) return -1;
    int yes = JS_ToBool(ctx, answer);
    JS_FreeValue(ctx, answer);
    return yes;
}

JSValue d_stream_new(os64_dom_t *dom, JSContext *ctx, const uint8_t *bytes, size_t length)
{
    JSValue chunk = d_uint8_array(ctx, length != 0 ? bytes : (const uint8_t *)"", length);
    if (JS_IsException(chunk)) return chunk;
    JSValue stream = door_call(dom, ctx, "fromBytes", 1, (JSValueConst *)&chunk);
    JS_FreeValue(ctx, chunk);
    return stream;
}

int d_stream_is(os64_dom_t *dom, JSContext *ctx, JSValueConst value)
{
    if (dom->streams_door == NULL || !JS_IsObject(value)) return 0;
    return door_bool(dom, ctx, "is", value);
}

int d_stream_locked(os64_dom_t *dom, JSContext *ctx, JSValueConst stream)
{
    return door_bool(dom, ctx, "locked", stream);
}

int d_stream_disturbed(os64_dom_t *dom, JSContext *ctx, JSValueConst stream)
{
    return door_bool(dom, ctx, "disturbed", stream);
}

int d_stream_readable(os64_dom_t *dom, JSContext *ctx, JSValueConst stream)
{
    return door_bool(dom, ctx, "readable", stream);
}

JSValue d_stream_spent(os64_dom_t *dom, JSContext *ctx, bool locked)
{
    JSValueConst argv[1] = {JS_NewBool(ctx, locked)};
    return door_call(dom, ctx, "spent", 1, argv);
}

int d_stream_take(os64_dom_t *dom, JSContext *ctx, JSValueConst stream)
{
    JSValue done = door_call(dom, ctx, "take", 1, &stream);
    if (JS_IsException(done)) return -1;
    JS_FreeValue(ctx, done);
    return 0;
}

JSValue d_stream_read_all_then(os64_dom_t *dom, JSContext *ctx, JSValueConst stream, JSValueConst convert)
{
    JSValueConst argv[2] = {stream, convert};
    return door_call(dom, ctx, "readAllThen", 2, argv);
}

int d_stream_tee(os64_dom_t *dom, JSContext *ctx, JSValueConst stream, JSValue out[2])
{
    out[0] = out[1] = JS_UNDEFINED;
    JSValue pair = door_call(dom, ctx, "tee", 1, &stream);
    if (JS_IsException(pair)) return -1;
    out[0] = JS_GetPropertyUint32(ctx, pair, 0);
    out[1] = JS_IsException(out[0]) ? JS_UNDEFINED : JS_GetPropertyUint32(ctx, pair, 1);
    JS_FreeValue(ctx, pair);
    if (JS_IsException(out[0]) || JS_IsException(out[1])) {
        JS_FreeValue(ctx, out[0]);
        JS_FreeValue(ctx, out[1]);
        out[0] = out[1] = JS_UNDEFINED;
        return -1;
    }
    return 0;
}

int d_stream_error(os64_dom_t *dom, JSContext *ctx, JSValueConst stream, JSValueConst reason)
{
    JSValueConst argv[2] = {stream, reason};
    JSValue done = door_call(dom, ctx, "error", 2, argv);
    if (JS_IsException(done)) return -1;
    JS_FreeValue(ctx, done);
    return 0;
}

JSValue d_stream_from_host(os64_dom_t *dom, JSContext *ctx, JSValueConst want, JSValueConst cancel)
{
    JSValueConst argv[2] = {want, cancel};
    return door_call(dom, ctx, "fromHost", 2, argv);
}

int d_stream_enqueue(os64_dom_t *dom, JSContext *ctx, JSValueConst stream, const uint8_t *bytes, size_t length)
{
    JSValue chunk = d_uint8_array(ctx, bytes, length);
    if (JS_IsException(chunk)) return -1;
    JSValueConst argv[2] = {stream, chunk};
    JSValue taken = door_call(dom, ctx, "enqueue", 2, argv);
    JS_FreeValue(ctx, chunk);
    if (JS_IsException(taken)) return -1;
    int yes = JS_ToBool(ctx, taken);
    JS_FreeValue(ctx, taken);
    return yes;
}

int d_stream_close(os64_dom_t *dom, JSContext *ctx, JSValueConst stream)
{
    JSValue done = door_call(dom, ctx, "close", 1, &stream);
    if (JS_IsException(done)) return -1;
    JS_FreeValue(ctx, done);
    return 0;
}
