#include "os64/js_engine.h"
#include "os64/mem.h"
#include "os64/io.h"
#include "os64/proc.h"
#include "os64/str.h"
#include "allocator.h"
#include "platform.h"
#include <limits.h>

#define JS_PUBLIC __attribute__((visibility("default")))

typedef struct Rejection {
    struct Rejection *next;
    JSValue promise, reason;
} Rejection;

struct os64_js_runtime {
    JSRuntime *engine;
    JSContext *context;
    JSPortAllocator allocator;
    os64_js_limits_t limits;
    os64_js_outcome_t *active_outcome;
    Rejection *rejections;
    int64_t deadline, host_error;
    uint64_t jobs;
    os64_js_status_t failure;
    os64_js_limit_t failed_limit;
    unsigned cancelled;
    bool active, turn, source_truncated;
    bool evaluated, output_installed, args_installed;
    int32_t output_handle;
    char source_name[OS64_JS_SOURCE_NAME_CAP];
};

static unsigned class_lock;

static bool copy_text(char *destination, size_t capacity, const char *source)
{
    size_t n = 0;
    if (source != NULL)
        while (n + 1 < capacity && source[n] != 0) {
            destination[n] = source[n];
            n++;
        }
    destination[n] = 0;
    return source != NULL && source[n] != 0;
}

static os64_js_status_t status(os64_js_outcome_t *out, os64_js_status_t value,
                                const char *message)
{
    out->status = value;
    if (message != NULL)
        out->diagnostic_truncated |= copy_text(out->message, sizeof(out->message), message);
    return value;
}

static void latch(os64_js_runtime_t *runtime, os64_js_status_t value,
                  os64_js_limit_t limit, int64_t error)
{
    if (runtime->failure == OS64_JS_OK) {
        runtime->failure = value;
        runtime->failed_limit = limit;
        runtime->host_error = error;
    }
}

/* An interrupt or allocator refusal is sticky even if script catches the
 * engine's exception. The host observes the structured verdict on return. */
static bool observe(os64_js_runtime_t *runtime)
{
    if (__atomic_load_n(&runtime->cancelled, __ATOMIC_ACQUIRE))
        latch(runtime, OS64_JS_CANCELLED, OS64_JS_LIMIT_NONE, 0);
    if (runtime->allocator.failures & JSPORT_ALLOC_LIMIT)
        latch(runtime, OS64_JS_LIMIT, OS64_JS_LIMIT_MEMORY, 0);
    if (runtime->allocator.failures & JSPORT_ALLOC_OOM)
        latch(runtime, OS64_JS_HOST_FAILURE, OS64_JS_LIMIT_NONE, 0);
    if (runtime->failure == OS64_JS_OK && runtime->turn) {
        int64_t now = os64_micros();
        if (now < 0)
            latch(runtime, OS64_JS_HOST_FAILURE, OS64_JS_LIMIT_NONE, now);
        else if (now >= runtime->deadline)
            latch(runtime, OS64_JS_LIMIT, OS64_JS_LIMIT_EXECUTION, 0);
    }
    return runtime->failure != OS64_JS_OK;
}

static int interrupt(JSRuntime *engine, void *opaque)
{
    (void)engine;
    return observe(opaque);
}

static void clear_rejections(os64_js_runtime_t *runtime)
{
    while (runtime->rejections != NULL) {
        Rejection *item = runtime->rejections;
        runtime->rejections = item->next;
        JS_FreeValueRT(runtime->engine, item->promise);
        JS_FreeValueRT(runtime->engine, item->reason);
        js_free_rt(runtime->engine, item);
    }
}

static void rejection(JSContext *context, JSValueConst promise,
                      JSValueConst reason, JS_BOOL handled, void *opaque)
{
    os64_js_runtime_t *runtime = opaque;
    Rejection **place = &runtime->rejections;
    while (*place != NULL) {
        if (observe(runtime)) return;
        if (JS_VALUE_GET_PTR((*place)->promise) == JS_VALUE_GET_PTR(promise)) {
            if (handled) {
                Rejection *item = *place;
                *place = item->next;
                JS_FreeValueRT(runtime->engine, item->promise);
                JS_FreeValueRT(runtime->engine, item->reason);
                js_free_rt(runtime->engine, item);
            }
            return;
        }
        place = &(*place)->next;
    }
    if (handled || observe(runtime)) return;
    Rejection *item = js_malloc_rt(runtime->engine, sizeof(*item));
    if (item == NULL) return;
    *item = (Rejection){.promise = JS_DupValue(context, promise),
                        .reason = JS_DupValue(context, reason)};
    *place = item;
}

static void discard_exception(JSContext *context)
{
    JSValue secondary = JS_GetException(context);
    JS_FreeValue(context, secondary);
}

/* Conversion and property access can execute script. Keep the original turn
 * armed and the public entry guard held while extracting diagnostics. */
static void diagnostic(os64_js_runtime_t *runtime, JSValueConst value,
                       os64_js_outcome_t *out)
{
    copy_text(out->message, sizeof(out->message), "JavaScript exception; diagnostic unavailable");
    if (observe(runtime)) return;
    const char *text = JS_ToCString(runtime->context, value);
    if (text != NULL) {
        out->diagnostic_truncated |= copy_text(out->message, sizeof(out->message), text);
        JS_FreeCString(runtime->context, text);
    } else {
        discard_exception(runtime->context);
    }
    if (!JS_IsObject(value) || observe(runtime)) return;
    JSValue stack = JS_GetPropertyStr(runtime->context, value, "stack");
    if (JS_IsException(stack)) {
        discard_exception(runtime->context);
    } else if (JS_IsString(stack)) {
        text = JS_ToCString(runtime->context, stack);
        if (text != NULL) {
            out->diagnostic_truncated |= copy_text(out->stack_trace, sizeof(out->stack_trace), text);
            JS_FreeCString(runtime->context, text);
        } else {
            discard_exception(runtime->context);
        }
    }
    JS_FreeValue(runtime->context, stack);
}

/* A checkpoint must not create more jobs by calling a rejection reason's
 * toString. Read string data on a real Error, or render a primitive; other
 * objects keep the fixed fallback. Error proxies do not pass JS_IsError. */
static void rejection_diagnostic(os64_js_runtime_t *runtime, JSValueConst value,
                                 os64_js_outcome_t *out)
{
    copy_text(out->message, sizeof(out->message), "unhandled Promise rejection");
    if (!JS_IsObject(value)) {
        diagnostic(runtime, value, out);
        return;
    }
    if (!JS_IsError(runtime->context, value) || observe(runtime)) return;
    const char *names[] = {"message", "stack"};
    char *fields[] = {out->message, out->stack_trace};
    const size_t sizes[] = {sizeof(out->message), sizeof(out->stack_trace)};
    for (size_t i = 0; i < 2 && !observe(runtime); i++) {
        JSAtom name = JS_NewAtom(runtime->context, names[i]);
        if (name == JS_ATOM_NULL) { discard_exception(runtime->context); break; }
        JSPropertyDescriptor descriptor = {0};
        int present = JS_GetOwnProperty(runtime->context, &descriptor, value, name);
        JS_FreeAtom(runtime->context, name);
        if (present < 0) discard_exception(runtime->context);
        if (present > 0) {
            if (JS_IsString(descriptor.value)) {
                const char *text = JS_ToCString(runtime->context, descriptor.value);
                if (text != NULL) {
                    out->diagnostic_truncated |= copy_text(fields[i], sizes[i], text);
                    JS_FreeCString(runtime->context, text);
                } else discard_exception(runtime->context);
            }
            JS_FreeValue(runtime->context, descriptor.value);
            JS_FreeValue(runtime->context, descriptor.getter);
            JS_FreeValue(runtime->context, descriptor.setter);
        }
    }
}

static os64_js_status_t enter(os64_js_runtime_t *runtime, os64_js_outcome_t *out)
{
    if (out == NULL) return OS64_JS_BAD_ARGUMENT;
    /* A callback must use its own outcome. Refuse an alias before clearing
     * storage that belongs to the active outer call. */
    if (runtime != NULL && runtime->active_outcome == out) return OS64_JS_BUSY;
    os64_memset(out, 0, sizeof(*out));
    if (runtime == NULL) return status(out, OS64_JS_BAD_ARGUMENT, "runtime is required");
    if (runtime->active) return status(out, OS64_JS_BUSY, "runtime is executing");
    if (runtime->failure != OS64_JS_OK)
        return status(out, OS64_JS_FAILED_RUNTIME, "runtime has failed");
    runtime->active = true;
    runtime->active_outcome = out;
    JS_UpdateStackTop(runtime->engine);
    return OS64_JS_OK;
}

static os64_js_status_t refuse(os64_js_runtime_t *runtime, os64_js_outcome_t *out,
                               os64_js_status_t value, const char *message)
{
    out->jobs_pending = JS_IsJobPending(runtime->engine);
    out->jobs_executed = runtime->jobs;
    runtime->active_outcome = NULL;
    runtime->active = false;
    return status(out, value, message);
}

static os64_js_status_t leave(os64_js_runtime_t *runtime, os64_js_outcome_t *out)
{
    bool pending = JS_IsJobPending(runtime->engine);
    if (pending && runtime->jobs == runtime->limits.jobs_per_turn)
        latch(runtime, OS64_JS_LIMIT, OS64_JS_LIMIT_JOBS, 0);
    if (!observe(runtime) && !pending && runtime->turn) {
        if (out->status == OS64_JS_OK && runtime->rejections != NULL) {
            out->status = OS64_JS_UNHANDLED_REJECTION;
            rejection_diagnostic(runtime, runtime->rejections->reason, out);
        }
        pending = JS_IsJobPending(runtime->engine);
        if (!pending) clear_rejections(runtime);
    }
    if (observe(runtime)) {
        const char *message = "host service failed";
        if (runtime->failure == OS64_JS_CANCELLED) message = "execution cancelled";
        else if (runtime->failure == OS64_JS_LIMIT) message = "runtime limit exceeded";
        else if (runtime->allocator.failures & JSPORT_ALLOC_OOM) message = "host allocation failed";
        status(out, runtime->failure, message);
        out->limit = runtime->failed_limit;
        out->host_error = runtime->host_error;
    }
    out->jobs_pending = pending;
    out->jobs_executed = runtime->jobs;
    out->diagnostic_truncated |= runtime->source_truncated |
        copy_text(out->source_name, sizeof(out->source_name), runtime->source_name);
    if (!pending || runtime->failure != OS64_JS_OK) runtime->turn = false;
    runtime->active_outcome = NULL;
    runtime->active = false;
    return out->status;
}

JS_PUBLIC os64_js_status_t os64_js_create(const os64_js_config_t *config,
                                        const char *caller_abi,
                                        os64_js_runtime_t **out,
                                        os64_js_outcome_t *outcome)
{
    if (out != NULL) *out = NULL;
    if (outcome == NULL) return OS64_JS_BAD_ARGUMENT;
    os64_memset(outcome, 0, sizeof(*outcome));
    if (out == NULL || config == NULL || caller_abi == NULL)
        return status(outcome, OS64_JS_BAD_ARGUMENT, "configuration, ABI and output are required");
    if (os64_strcmp(caller_abi, OS64_JS_ABI_ID) != 0)
        return status(outcome, OS64_JS_ABI_MISMATCH, "runtime header ABI mismatch");
    const os64_js_limits_t *limits = &config->limits;
    if (limits->memory_bytes == 0 || limits->stack_bytes == 0 ||
        limits->stack_bytes > PTRDIFF_MAX || limits->source_bytes == 0 ||
        limits->source_bytes == SIZE_MAX || limits->execution_ms == 0 ||
        limits->execution_ms > INT64_MAX / 1000 || limits->jobs_per_turn == 0)
        return status(outcome, OS64_JS_BAD_ARGUMENT, "limits must be positive and representable");
    int64_t now = os64_micros();
    if (now < 0) {
        outcome->host_error = now;
        return status(outcome, OS64_JS_HOST_FAILURE, "monotonic clock unavailable");
    }
    if (limits->execution_ms * 1000 > (uint64_t)(INT64_MAX - now))
        return status(outcome, OS64_JS_BAD_ARGUMENT, "deadline is not representable");
    /* Keep subtraction of the configured downward-growing stack budget away
     * from zero, including the engine frames below this construction frame. */
    if (limits->stack_bytes > (uintptr_t)&now / 2)
        return status(outcome, OS64_JS_BAD_ARGUMENT, "stack limit exceeds the address range");
    if (sizeof(os64_js_runtime_t) >= limits->memory_bytes) {
        outcome->limit = OS64_JS_LIMIT_MEMORY;
        return status(outcome, OS64_JS_LIMIT, "runtime storage exceeds memory limit");
    }
    os64_js_runtime_t *runtime = os64_malloc(sizeof(*runtime));
    if (runtime == NULL)
        return status(outcome, OS64_JS_HOST_FAILURE, "host allocation failed");
    size_t storage = os64_malloc_size(runtime);
    os64_memset(runtime, 0, sizeof(*runtime));
    runtime->limits = *limits;
    runtime->allocator.payload_limit = storage < limits->memory_bytes
        ? limits->memory_bytes - storage : 0;
    if (storage < limits->memory_bytes)
        runtime->engine = JS_NewRuntime2(&jsport_malloc_functions, &runtime->allocator);
    if (runtime->engine != NULL) {
        JS_SetMemoryLimit(runtime->engine, runtime->allocator.payload_limit);
        JS_SetMaxStackSize(runtime->engine, limits->stack_bytes);
        JS_SetRuntimeOpaque(runtime->engine, runtime);
        runtime->context = JS_NewContext(runtime->engine);
    }
    bool ready = false;
    if (runtime->context != NULL) {
        JSValue global = JS_GetGlobalObject(runtime->context);
        JSAtom name = JS_NewAtom(runtime->context, "SharedArrayBuffer");
        if (name != JS_ATOM_NULL)
            ready = JS_DeleteProperty(runtime->context, global, name, JS_PROP_THROW) == 1;
        JS_FreeAtom(runtime->context, name);
        JS_FreeValue(runtime->context, global);
    }
    if (!ready || runtime->allocator.failures != 0) {
        bool limited = storage >= limits->memory_bytes ||
            (runtime->allocator.failures & JSPORT_ALLOC_LIMIT);
        if (runtime->context != NULL) JS_FreeContext(runtime->context);
        if (runtime->engine != NULL) JS_FreeRuntime(runtime->engine);
        os64_free(runtime);
        if (limited) outcome->limit = OS64_JS_LIMIT_MEMORY;
        return status(outcome, limited ? OS64_JS_LIMIT : OS64_JS_HOST_FAILURE,
                      limited ? "runtime construction exceeds memory limit" : "runtime construction failed");
    }
    JS_SetContextOpaque(runtime->context, runtime);
    JS_SetInterruptHandler(runtime->engine, interrupt, runtime);
    JS_SetHostPromiseRejectionTracker(runtime->engine, rejection, runtime);
    *out = runtime;
    return OS64_JS_OK;
}

JS_PUBLIC JSContext *os64_js_context(os64_js_runtime_t *runtime, const char *caller_abi,
                                    os64_js_outcome_t *outcome)
{
    if (outcome == NULL || (runtime != NULL && runtime->active_outcome == outcome)) return NULL;
    os64_memset(outcome, 0, sizeof(*outcome));
    if (runtime == NULL || caller_abi == NULL) {
        status(outcome, OS64_JS_BAD_ARGUMENT, "runtime and ABI are required");
        return NULL;
    }
    if (os64_strcmp(caller_abi, OS64_JS_ABI_ID) != 0) {
        status(outcome, OS64_JS_ABI_MISMATCH, "binding header ABI mismatch");
        return NULL;
    }
    if (runtime->failure != OS64_JS_OK || __atomic_load_n(&runtime->cancelled, __ATOMIC_ACQUIRE)) {
        status(outcome, OS64_JS_FAILED_RUNTIME, "runtime has failed");
        return NULL;
    }
    return runtime->context;
}

JS_PUBLIC JSClassID os64_js_class_id(JSClassID *slot)
{
    if (slot == NULL) return 0;
    while (__atomic_exchange_n(&class_lock, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
    JSClassID id = JS_NewClassID(slot);
    __atomic_store_n(&class_lock, 0, __ATOMIC_RELEASE);
    return id;
}

/* Both buffer and file entry points own an accounted, terminated source here.
 * Loading and source copying finish before the execution deadline starts. */
static os64_js_status_t evaluate(os64_js_runtime_t *runtime, const char *source,
                                size_t length, const char *source_name,
                                os64_js_outcome_t *outcome)
{
    int64_t now = os64_micros();
    if (now < 0) latch(runtime, OS64_JS_HOST_FAILURE, OS64_JS_LIMIT_NONE, now);
    else if (runtime->limits.execution_ms * 1000 > (uint64_t)(INT64_MAX - now))
        return OS64_JS_BAD_ARGUMENT;
    else {
        runtime->jobs = 0;
        runtime->source_truncated = copy_text(runtime->source_name, sizeof(runtime->source_name), source_name);
        runtime->deadline = now + (int64_t)(runtime->limits.execution_ms * 1000);
        runtime->turn = true;
        runtime->evaluated = true;
        JSValue value = JS_Eval(runtime->context, source, length, source_name, JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(value)) {
            JSValue error = JS_GetException(runtime->context);
            outcome->status = OS64_JS_EXCEPTION;
            diagnostic(runtime, error, outcome);
            JS_FreeValue(runtime->context, error);
        }
        JS_FreeValue(runtime->context, value);
    }
    return OS64_JS_OK;
}

JS_PUBLIC os64_js_status_t os64_js_eval(os64_js_runtime_t *runtime,
                                      const void *source, size_t length,
                                      const char *source_name,
                                      os64_js_outcome_t *outcome)
{
    os64_js_status_t result = enter(runtime, outcome);
    if (result != OS64_JS_OK) return result;
    if ((source == NULL && length != 0) || source_name == NULL) {
        return refuse(runtime, outcome, OS64_JS_BAD_ARGUMENT, "source bytes and name are required");
    }
    if (runtime->turn || JS_IsJobPending(runtime->engine)) {
        return refuse(runtime, outcome, OS64_JS_BUSY, "the previous turn has queued jobs");
    }
    if (length > runtime->limits.source_bytes) {
        runtime->jobs = 0;
        runtime->source_truncated = copy_text(runtime->source_name, sizeof(runtime->source_name), source_name);
        latch(runtime, OS64_JS_LIMIT, OS64_JS_LIMIT_SOURCE, 0);
        return leave(runtime, outcome);
    }
    if (observe(runtime)) return leave(runtime, outcome);
    char *copy = js_malloc_rt(runtime->engine, length + 1);
    if (copy == NULL) return leave(runtime, outcome);
    if (length != 0) os64_memcpy(copy, source, length);
    copy[length] = 0;
    result = evaluate(runtime, copy, length, source_name, outcome);
    js_free_rt(runtime->engine, copy);
    if (result == OS64_JS_BAD_ARGUMENT)
        return refuse(runtime, outcome, result, "deadline is not representable");
    return leave(runtime, outcome);
}

JS_PUBLIC os64_js_status_t os64_js_drain_jobs(os64_js_runtime_t *runtime,
                                            uint64_t slice_jobs,
                                            os64_js_outcome_t *outcome)
{
    os64_js_status_t result = enter(runtime, outcome);
    if (result != OS64_JS_OK) return result;
    if (slice_jobs == 0) {
        return refuse(runtime, outcome, OS64_JS_BAD_ARGUMENT, "job slice must be positive");
    }
    if (!runtime->turn && JS_IsJobPending(runtime->engine))
        jsport_fatal("jobs outside a runtime turn", __FILE__, __LINE__);
    uint64_t count = 0;
    while (JS_IsJobPending(runtime->engine) && count < slice_jobs && !observe(runtime)) {
        if (runtime->jobs == runtime->limits.jobs_per_turn) {
            latch(runtime, OS64_JS_LIMIT, OS64_JS_LIMIT_JOBS, 0);
            break;
        }
        runtime->jobs++;
        count++;
        JSContext *context = NULL;
        if (JS_ExecutePendingJob(runtime->engine, &context) < 0) {
            if (context == NULL) context = runtime->context;
            JSValue error = JS_GetException(context);
            outcome->status = OS64_JS_EXCEPTION;
            diagnostic(runtime, error, outcome);
            JS_FreeValue(context, error);
            break;
        }
    }
    if (outcome->status == OS64_JS_OK && JS_IsJobPending(runtime->engine))
        outcome->status = OS64_JS_MORE_JOBS;
    return leave(runtime, outcome);
}

static os64_js_status_t complete_run(os64_js_runtime_t *runtime,
                                    os64_js_status_t result,
                                    os64_js_outcome_t *outcome)
{
    if (result != OS64_JS_OK) return result;
    while (outcome->jobs_pending) {
        result = os64_js_drain_jobs(runtime, UINT64_MAX, outcome);
        if (result != OS64_JS_OK && result != OS64_JS_MORE_JOBS) return result;
    }
    return result;
}

JS_PUBLIC os64_js_status_t os64_js_run(os64_js_runtime_t *runtime,
                                     const void *source, size_t length,
                                     const char *source_name,
                                     os64_js_outcome_t *outcome)
{
    return complete_run(runtime, os64_js_eval(runtime, source, length, source_name, outcome), outcome);
}

JS_PUBLIC os64_js_status_t os64_js_run_file(os64_js_runtime_t *runtime,
                                          const char *path,
                                          os64_js_outcome_t *outcome)
{
    os64_js_status_t result = enter(runtime, outcome);
    if (result != OS64_JS_OK) return result;
    if (path == NULL || path[0] == 0)
        return refuse(runtime, outcome, OS64_JS_BAD_ARGUMENT, "file path is required");
    if (runtime->turn || JS_IsJobPending(runtime->engine))
        return refuse(runtime, outcome, OS64_JS_BUSY, "the previous turn has queued jobs");
    if (observe(runtime)) return leave(runtime, outcome);
    int64_t opened = os64_open(path, "r");
    if (opened < 0) {
        latch(runtime, OS64_JS_HOST_FAILURE, OS64_JS_LIMIT_NONE, opened);
        runtime->jobs = 0;
        runtime->source_truncated = copy_text(runtime->source_name, sizeof(runtime->source_name), path);
        return leave(runtime, outcome);
    }
    size_t capacity = runtime->limits.source_bytes < 4096
        ? runtime->limits.source_bytes + 1 : 4096;
    size_t length = 0;
    char *source = js_malloc_rt(runtime->engine, capacity);
    while (source != NULL && !observe(runtime)) {
        if (length == runtime->limits.source_bytes) {
            char extra;
            int64_t n = os64_read((int32_t)opened, &extra, 1);
            if (n < 0) latch(runtime, OS64_JS_HOST_FAILURE, OS64_JS_LIMIT_NONE, n);
            else if (n != 0) latch(runtime, OS64_JS_LIMIT, OS64_JS_LIMIT_SOURCE, 0);
            break;
        }
        if (length + 1 == capacity) {
            size_t ceiling = runtime->limits.source_bytes + 1;
            size_t next = capacity > ceiling / 2 ? ceiling : capacity * 2;
            char *grown = js_realloc_rt(runtime->engine, source, next);
            if (grown == NULL) break;
            source = grown;
            capacity = next;
        }
        size_t room = capacity - length - 1;
        int64_t n = os64_read((int32_t)opened, source + length, room);
        if (n < 0) { latch(runtime, OS64_JS_HOST_FAILURE, OS64_JS_LIMIT_NONE, n); break; }
        if (n == 0) break;
        if ((uint64_t)n > room) {
            latch(runtime, OS64_JS_HOST_FAILURE, OS64_JS_LIMIT_NONE, 0);
            break;
        }
        length += (size_t)n;
    }
    observe(runtime);
    /* Close consumes this owned input even after a read/allocation refusal.
     * A nonzero close verdict is failure; keep the first failure. */
    int64_t closed = os64_close((int32_t)opened);
    if (closed != 0) latch(runtime, OS64_JS_HOST_FAILURE, OS64_JS_LIMIT_NONE, closed);
    if (source != NULL && !observe(runtime)) {
        source[length] = 0;
        result = evaluate(runtime, source, length, path, outcome);
    }
    js_free_rt(runtime->engine, source);
    if (result == OS64_JS_BAD_ARGUMENT)
        return refuse(runtime, outcome, result, "deadline is not representable");
    if (runtime->failure != OS64_JS_OK && !runtime->turn) {
        runtime->jobs = 0;
        runtime->source_truncated = copy_text(runtime->source_name, sizeof(runtime->source_name), path);
    }
    return complete_run(runtime, leave(runtime, outcome), outcome);
}

static bool write_output(os64_js_runtime_t *runtime, const char *bytes, size_t size)
{
    while (size != 0 && !observe(runtime)) {
        int64_t n = os64_write(runtime->output_handle, bytes, size);
        if (n <= 0 || (uint64_t)n > size) {
            latch(runtime, OS64_JS_HOST_FAILURE, OS64_JS_LIMIT_NONE, n < 0 ? n : 0);
            return false;
        }
        bytes += (size_t)n;
        size -= (size_t)n;
    }
    return !observe(runtime);
}

static JSValue output_call(JSContext *context, JSValueConst self, int argc,
                           JSValueConst *args)
{
    (void)self;
    os64_js_runtime_t *runtime = JS_GetContextOpaque(context);
    if (!runtime->active || !runtime->turn)
        jsport_fatal("output outside a runtime turn", __FILE__, __LINE__);
    /* Property traps can see a staged function before setup commits. It must
     * not use a borrowed handle until the entire installation succeeds. */
    if (!runtime->output_installed)
        return JS_ThrowTypeError(context, "output installation is incomplete");
    for (int i = 0; i < argc && !observe(runtime); i++) {
        size_t length;
        const char *text = JS_ToCStringLen(context, &length, args[i]);
        if (text == NULL) return JS_EXCEPTION;
        bool written = (i == 0 || write_output(runtime, " ", 1)) &&
                       write_output(runtime, text, length);
        JS_FreeCString(context, text);
        if (!written) break;
    }
    if (write_output(runtime, "\n", 1)) return JS_UNDEFINED;
    return JS_ThrowInternalError(context, "host output failed");
}

/* Setup may reach a host-supplied console object's property traps. Give it
 * a guarded budget without marking the first source evaluation as started. */
static os64_js_status_t setup(os64_js_runtime_t *runtime, bool installed,
                             os64_js_outcome_t *outcome, const char *name)
{
    if (runtime->evaluated || installed || runtime->turn || JS_IsJobPending(runtime->engine))
        return refuse(runtime, outcome, OS64_JS_BAD_ARGUMENT, "installer requires unused idle setup");
    int64_t now = os64_micros();
    if (now < 0) latch(runtime, OS64_JS_HOST_FAILURE, OS64_JS_LIMIT_NONE, now);
    else if (runtime->limits.execution_ms * 1000 > (uint64_t)(INT64_MAX - now))
        return refuse(runtime, outcome, OS64_JS_BAD_ARGUMENT, "deadline is not representable");
    else {
        runtime->deadline = now + (int64_t)(runtime->limits.execution_ms * 1000);
        runtime->turn = true;
        runtime->jobs = 0;
        runtime->source_truncated = copy_text(runtime->source_name, sizeof(runtime->source_name), name);
    }
    return OS64_JS_OK;
}

static void setup_exception(os64_js_runtime_t *runtime, os64_js_outcome_t *outcome)
{
    JSValue error = JS_GetException(runtime->context);
    outcome->status = OS64_JS_EXCEPTION;
    diagnostic(runtime, error, outcome);
    JS_FreeValue(runtime->context, error);
}

typedef struct {
    JSValue object, value;
    JSAtom name;
    JSPropertyDescriptor previous;
    int present;
    bool attempted;
} OutputProperty;

static void free_descriptor(JSContext *context, JSPropertyDescriptor *descriptor)
{
    JS_FreeValue(context, descriptor->value);
    JS_FreeValue(context, descriptor->getter);
    JS_FreeValue(context, descriptor->setter);
}

/* Retain the complete descriptor before changing it. Validation and function
 * allocation precede publication; no console getter is used for discovery. */
static int prepare_output_property(JSContext *context, OutputProperty *property,
                                   JSValueConst object, const char *name, JSValue value)
{
    property->object = object;
    property->value = value;
    if (JS_IsException(value)) return -1;
    property->name = JS_NewAtom(context, name);
    if (property->name == JS_ATOM_NULL) return -1;
    property->present = JS_GetOwnProperty(context, &property->previous, object, property->name);
    if (property->present < 0) return -1;
    if (property->present > 0 && !(property->previous.flags & JS_PROP_CONFIGURABLE)) {
        JS_ThrowTypeError(context, "output property is not configurable");
        return -1;
    }
    if (property->present == 0) {
        int extensible = JS_IsExtensible(context, object);
        if (extensible < 0) return -1;
        if (!extensible) {
            JS_ThrowTypeError(context, "output object is not extensible");
            return -1;
        }
    }
    return 0;
}

static void restore_output_property(os64_js_runtime_t *runtime, OutputProperty *property)
{
    JSContext *context = runtime->context;
    int restored;
    if (property->present == 0) {
        restored = JS_DeleteProperty(context, property->object, property->name, JS_PROP_THROW);
    } else {
        const JSPropertyDescriptor *old = &property->previous;
        int flags = (old->flags & JS_PROP_C_W_E) | JS_PROP_HAS_CONFIGURABLE |
                    JS_PROP_HAS_ENUMERABLE | JS_PROP_THROW;
        if (old->flags & JS_PROP_GETSET) flags |= JS_PROP_HAS_GET | JS_PROP_HAS_SET;
        else flags |= JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE;
        restored = JS_DefineProperty(context, property->object, property->name,
                                    old->value, old->getter, old->setter, flags);
    }
    if (restored <= 0) {
        discard_exception(context);
        observe(runtime);
        /* A host-supplied trap or allocation refusal may prevent restoration.
         * Retire instead of exposing a reusable partially installed runtime. */
        latch(runtime, OS64_JS_HOST_FAILURE, OS64_JS_LIMIT_NONE, 0);
    }
}

JS_PUBLIC os64_js_status_t os64_js_install_output(os64_js_runtime_t *runtime,
                                                int32_t handle, uint32_t names,
                                                os64_js_outcome_t *outcome)
{
    os64_js_status_t result = enter(runtime, outcome);
    if (result != OS64_JS_OK) return result;
    const uint32_t valid = OS64_JS_OUTPUT_PRINT | OS64_JS_OUTPUT_CONSOLE_LOG;
    if (handle < 0 || names == 0 || (names & ~valid) != 0)
        return refuse(runtime, outcome, OS64_JS_BAD_ARGUMENT, "output handle and selected names are required");
    result = setup(runtime, runtime->output_installed, outcome, "<output setup>");
    if (result != OS64_JS_OK) return result;
    if (observe(runtime)) return leave(runtime, outcome);
    JSContext *context = runtime->context;
    JSValue global = JS_GetGlobalObject(context), console = JS_UNDEFINED;
    JSPropertyDescriptor console_descriptor = {0};
    OutputProperty properties[3] = {0};
    size_t count = 0;
    int console_present = 0;
    int installed = 0;
    if (names & OS64_JS_OUTPUT_CONSOLE_LOG) {
        JSAtom name = JS_NewAtom(context, "console");
        console_present = name == JS_ATOM_NULL ? -1 : JS_GetOwnProperty(context, &console_descriptor, global, name);
        JS_FreeAtom(context, name);
        if (console_present == 0) console = JS_NewObject(context);
        else if (console_present > 0) {
            if (JS_IsObject(console_descriptor.value)) console = JS_DupValue(context, console_descriptor.value);
            else console = JS_ThrowTypeError(context, "console must be an own data object");
        }
        if (console_present < 0 || JS_IsException(console)) installed = -1;
        else installed = prepare_output_property(context, &properties[count++], console, "log",
                          JS_NewCFunction(context, output_call, "log", 1));
    }
    if (installed >= 0 && (names & OS64_JS_OUTPUT_PRINT))
        installed = prepare_output_property(context, &properties[count++], global, "print",
                      JS_NewCFunction(context, output_call, "print", 1));
    if (installed >= 0 && (names & OS64_JS_OUTPUT_CONSOLE_LOG) && console_present == 0)
        installed = prepare_output_property(context, &properties[count++], global, "console",
                      JS_DupValue(context, console));
    for (size_t i = 0; installed >= 0 && i < count && !observe(runtime); i++) {
        OutputProperty *property = &properties[i];
        /* DefineProperty may change the value before a later flags allocation
         * fails, so an attempted write also needs restoration on refusal. */
        property->attempted = true;
        installed = JS_DefineProperty(context, property->object, property->name, property->value,
                      JS_UNDEFINED, JS_UNDEFINED, JS_PROP_C_W_E | JS_PROP_HAS_VALUE |
                      JS_PROP_HAS_CONFIGURABLE | JS_PROP_HAS_WRITABLE | JS_PROP_HAS_ENUMERABLE | JS_PROP_THROW);
        if (installed == 0) installed = -1;
    }
    if (installed < 0 || observe(runtime)) {
        JSValue error = JS_GetException(context);
        for (size_t i = count; i != 0; i--)
            if (properties[i-1].attempted) restore_output_property(runtime, &properties[i-1]);
        if (installed < 0) {
            outcome->status = OS64_JS_EXCEPTION;
            diagnostic(runtime, error, outcome);
        }
        JS_FreeValue(context, error);
    } else {
        runtime->output_handle = handle;
        runtime->output_installed = true;
    }
    for (size_t i = 0; i < count; i++) {
        OutputProperty *property = &properties[i];
        if (property->present > 0) free_descriptor(context, &property->previous);
        JS_FreeAtom(context, property->name);
        JS_FreeValue(context, property->value);
    }
    if (console_present > 0) free_descriptor(context, &console_descriptor);
    JS_FreeValue(context, console);
    JS_FreeValue(context, global);
    return leave(runtime, outcome);
}

JS_PUBLIC os64_js_status_t os64_js_install_args(os64_js_runtime_t *runtime,
                                              size_t count, const char *const *args,
                                              os64_js_outcome_t *outcome)
{
    os64_js_status_t result = enter(runtime, outcome);
    if (result != OS64_JS_OK) return result;
    if (count > UINT32_MAX || (count != 0 && args == NULL))
        return refuse(runtime, outcome, OS64_JS_BAD_ARGUMENT, "argument array is invalid");
    for (size_t i = 0; i < count; i++)
        if (args[i] == NULL)
            return refuse(runtime, outcome, OS64_JS_BAD_ARGUMENT, "argument entries must be strings");
    result = setup(runtime, runtime->args_installed, outcome, "<argument setup>");
    if (result != OS64_JS_OK) return result;
    if (observe(runtime)) return leave(runtime, outcome);
    JSContext *context = runtime->context;
    JSValue array = JS_NewArray(context);
    int installed = JS_IsException(array) ? -1 : 0;
    for (size_t i = 0; installed >= 0 && i < count && !observe(runtime); i++) {
        JSValue text = JS_NewString(context, args[i]);
        if (JS_IsException(text)) installed = -1;
        else installed = JS_DefinePropertyValueUint32(context, array, (uint32_t)i, text,
                                                      JS_PROP_C_W_E | JS_PROP_THROW);
    }
    if (installed >= 0 && !observe(runtime)) {
        JSValue global = JS_GetGlobalObject(context);
        installed = JS_DefinePropertyValueStr(context, global, "scriptArgs", array, JS_PROP_C_W_E | JS_PROP_THROW);
        array = JS_UNDEFINED;
        JS_FreeValue(context, global);
    }
    JS_FreeValue(context, array);
    if (installed < 0) setup_exception(runtime, outcome);
    else if (!observe(runtime)) runtime->args_installed = true;
    return leave(runtime, outcome);
}

JS_PUBLIC void os64_js_cancel(os64_js_runtime_t *runtime)
{
    if (runtime != NULL) __atomic_store_n(&runtime->cancelled, 1, __ATOMIC_RELEASE);
}

JS_PUBLIC void os64_js_destroy(os64_js_runtime_t *runtime)
{
    if (runtime == NULL) return;
    if (runtime->active) jsport_fatal("destroy during an active runtime call", __FILE__, __LINE__);
    runtime->active = true;
    runtime->failure = OS64_JS_FAILED_RUNTIME;
    JS_SetHostPromiseRejectionTracker(runtime->engine, NULL, NULL);
    clear_rejections(runtime);
    JS_FreeContext(runtime->context);
    JS_FreeRuntime(runtime->engine);
    os64_free(runtime);
}
