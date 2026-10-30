#include "os64/js_engine.h"
#include "os64/mem.h"
#include "os64/io.h"
#include "os64/proc.h"
#include "os64/str.h"
#include "os64/fmt.h"
#include "allocator.h"
#include "platform.h"
#include "os64_js_engine_id.h"
#include <limits.h>

#define JS_PUBLIC __attribute__((visibility("default")))

/* A rejected promise nothing handles yet: oldest first on a list, and found
 * by its promise in a table, so a page's thousandth rejection or handler
 * costs what its first did. */
typedef struct Rejection {
    struct Rejection *next, *prev;
    JSValue promise, reason;
    uint32_t tag;           // the script host's answer when it was rejected
} Rejection;

struct os64_js_runtime {
    JSRuntime *engine;
    JSContext *context;
    JSPortAllocator allocator;
    os64_js_limits_t limits;
    os64_js_outcome_t *active_outcome;
    Rejection *rejections, *rejections_last;
    /* Open addressing over the promises' addresses, a power of two in
     * size; `used` counts the removed slots too, which a growth clears. */
    Rejection **rejection_table;
    size_t rejection_capacity, rejection_count, rejection_used;
    /* A host that reports script failures itself (os64_js_set_script_host):
     * with one, rejections wait for os64_js_take_rejection rather than
     * being judged at a checkpoint. */
    os64_js_script_host_t host;
    bool hosted;
    int64_t deadline, host_error;
    uint64_t jobs;
    os64_js_status_t failure;
    os64_js_limit_t failed_limit;
    unsigned cancelled;
    /* A host task holds one turn open across several calls and checkpoints;
     * rejections are judged only at a checkpoint while it is open. */
    bool active, turn, task, checkpointing, source_truncated;
    bool evaluated, output_installed, args_installed;
    int32_t output_handle;
    /* console.group's nesting: each console line is indented by it. */
    uint32_t console_depth;
    char source_name[OS64_JS_SOURCE_NAME_CAP];
    /* The host's module handles (§ Modules), and the loader the engine asks
     * for a module it cannot find among the ones compiled. */
    struct os64_js_module *modules;
    os64_js_module_loader_t loader;
};

/* A module the host compiled: a reference of its own to the engine's
 * module, and once run, its evaluation's promise; a rejection of that
 * promise is the module's failure, kept here as its outcome rather than
 * judged as an unhandled rejection (HTML reports it as an error). */
struct os64_js_module {
    struct os64_js_module *next, *prev;
    JSValue module, promise;
    bool run, rejected;
    os64_js_outcome_t failure;
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

/* The table's removed slot: no Rejection lives at this address. */
static Rejection rejection_removed;

static size_t rejection_slot(const os64_js_runtime_t *runtime, const void *promise)
{
    uint64_t key = (uint64_t)(uintptr_t)promise * 0x9E3779B97F4A7C15ull;
    return (size_t)(key >> 32) & (runtime->rejection_capacity - 1);
}

static Rejection **rejection_place(os64_js_runtime_t *runtime, const void *promise)
{
    if (runtime->rejection_capacity == 0) return NULL;
    for (size_t at = rejection_slot(runtime, promise);; at = (at + 1) & (runtime->rejection_capacity - 1)) {
        Rejection *item = runtime->rejection_table[at];
        if (item == NULL) return NULL;
        if (item != &rejection_removed && JS_VALUE_GET_PTR(item->promise) == promise)
            return &runtime->rejection_table[at];
    }
}

/* Room for one more, keeping the table at most half full of live and
 * removed slots; a growth rebuilds it from the list. False when the engine
 * would not give the memory (the rejection then goes untracked). */
static bool rejection_room(os64_js_runtime_t *runtime)
{
    if ((runtime->rejection_used + 1) * 2 <= runtime->rejection_capacity) return true;
    size_t capacity = 16;
    while (capacity < (runtime->rejection_count + 1) * 4) capacity *= 2;
    Rejection **table = js_mallocz_rt(runtime->engine, capacity * sizeof(*table));
    if (table == NULL) return false;
    js_free_rt(runtime->engine, runtime->rejection_table);
    runtime->rejection_table = table;
    runtime->rejection_capacity = capacity;
    runtime->rejection_used = runtime->rejection_count;
    for (Rejection *item = runtime->rejections; item != NULL; item = item->next) {
        size_t at = rejection_slot(runtime, JS_VALUE_GET_PTR(item->promise));
        while (table[at] != NULL) at = (at + 1) & (capacity - 1);
        table[at] = item;
    }
    return true;
}

/* Off the list and out of the table; its references are the caller's. */
static void rejection_unlink(os64_js_runtime_t *runtime, Rejection **place)
{
    Rejection *item = *place;
    *place = &rejection_removed;
    runtime->rejection_count--;
    if (item->prev != NULL) item->prev->next = item->next;
    else runtime->rejections = item->next;
    if (item->next != NULL) item->next->prev = item->prev;
    else runtime->rejections_last = item->prev;
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
    runtime->rejections_last = NULL;
    js_free_rt(runtime->engine, runtime->rejection_table);
    runtime->rejection_table = NULL;
    runtime->rejection_capacity = runtime->rejection_count = runtime->rejection_used = 0;
}

static char *module_normalize(JSContext *context, const char *base, const char *specifier, void *opaque);
static JSModuleDef *module_load(JSContext *context, const char *name, void *opaque);
static void module_free(os64_js_runtime_t *runtime, struct os64_js_module *module);
static void caught(os64_js_runtime_t *runtime, JSContext *context, JSValueConst error);
static void rejection_diagnostic(os64_js_runtime_t *runtime, JSValueConst value,
                                 os64_js_outcome_t *out);

/* The handle whose evaluation `promise` is, or NULL. */
static struct os64_js_module *module_of(os64_js_runtime_t *runtime, JSValueConst promise)
{
    for (struct os64_js_module *m = runtime->modules; m != NULL; m = m->next)
        if (m->run && JS_VALUE_GET_TAG(m->promise) == JS_TAG_OBJECT &&
            JS_VALUE_GET_PTR(m->promise) == JS_VALUE_GET_PTR(promise))
            return m;
    return NULL;
}

/* A module's evaluation rejected: its failure, kept for os64_js_module_state
 * and reported as an exception (the script host's `exception`), which is
 * what HTML does with a module script's error. Runs no script. */
static void module_failed(os64_js_runtime_t *runtime, JSContext *context,
                          struct os64_js_module *module, JSValueConst reason)
{
    if (module->rejected) return;
    module->rejected = true;
    os64_memset(&module->failure, 0, sizeof(module->failure));
    module->failure.status = OS64_JS_EXCEPTION;
    rejection_diagnostic(runtime, reason, &module->failure);
    // Its fallback names an unhandled rejection, which this is not.
    if (os64_strcmp(module->failure.message, "unhandled Promise rejection") == 0)
        copy_text(module->failure.message, sizeof(module->failure.message), "module evaluation failed");
    caught(runtime, context, reason);
}

/* The engine's tracker: a promise rejected with no handler, or a handler
 * added to one. A handler for a promise this table no longer holds, which a
 * host took (os64_js_take_rejection), is the host's to hear of. A module's
 * evaluation is its handle's (module_failed), never an unhandled rejection. */
static void rejection(JSContext *context, JSValueConst promise,
                      JSValueConst reason, JS_BOOL handled, void *opaque)
{
    os64_js_runtime_t *runtime = opaque;
    if (observe(runtime)) return;
    struct os64_js_module *module = handled ? NULL : module_of(runtime, promise);
    if (module != NULL) {
        module_failed(runtime, context, module, reason);
        return;
    }
    Rejection **place = rejection_place(runtime, JS_VALUE_GET_PTR(promise));
    if (place != NULL) {
        if (handled) {
            Rejection *item = *place;
            rejection_unlink(runtime, place);
            JS_FreeValueRT(runtime->engine, item->promise);
            JS_FreeValueRT(runtime->engine, item->reason);
            js_free_rt(runtime->engine, item);
        }
        return;
    }
    if (handled) {
        if (runtime->hosted && runtime->host.handled != NULL)
            runtime->host.handled(runtime->host.opaque, context, promise);
        return;
    }
    if (!rejection_room(runtime)) return;
    Rejection *item = js_malloc_rt(runtime->engine, sizeof(*item));
    if (item == NULL) return;
    *item = (Rejection){.promise = JS_DupValue(context, promise),
                        .reason = JS_DupValue(context, reason),
                        .prev = runtime->rejections_last};
    if (runtime->rejections_last != NULL) runtime->rejections_last->next = item;
    else runtime->rejections = item;
    runtime->rejections_last = item;
    size_t at = rejection_slot(runtime, JS_VALUE_GET_PTR(promise));
    while (runtime->rejection_table[at] != NULL && runtime->rejection_table[at] != &rejection_removed)
        at = (at + 1) & (runtime->rejection_capacity - 1);
    if (runtime->rejection_table[at] == NULL) runtime->rejection_used++;
    runtime->rejection_table[at] = item;
    runtime->rejection_count++;
    /* Asked now, while the script that rejected may still be on the stack;
     * kept with the rejection for the host that takes it. */
    if (runtime->hosted && runtime->host.rejected != NULL)
        item->tag = runtime->host.rejected(runtime->host.opaque, context, promise);
}

/* An exception the wrapper caught, for a host that reports script failures
 * itself; never one a sticky failure threw. */
static void caught(os64_js_runtime_t *runtime, JSContext *context, JSValueConst error)
{
    if (runtime->hosted && runtime->host.exception != NULL && !observe(runtime))
        runtime->host.exception(runtime->host.opaque, context, error);
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

static void sticky(os64_js_runtime_t *runtime, os64_js_outcome_t *out)
{
    const char *message = "host service failed";
    if (runtime->failure == OS64_JS_CANCELLED) message = "execution cancelled";
    else if (runtime->failure == OS64_JS_LIMIT) message = "runtime limit exceeded";
    else if (runtime->allocator.failures & JSPORT_ALLOC_OOM) message = "host allocation failed";
    status(out, runtime->failure, message);
    out->limit = runtime->failed_limit;
    out->host_error = runtime->host_error;
}

/* A turn ends at queue exhaustion, except that an open task keeps its turn
 * (and so its deadline and job count) until os64_js_task_end closes it.
 * Without a script host, that is where rejections are judged; with one,
 * they wait for the host to take them. */
static os64_js_status_t leave(os64_js_runtime_t *runtime, os64_js_outcome_t *out)
{
    bool pending = JS_IsJobPending(runtime->engine);
    if (pending && runtime->jobs == runtime->limits.jobs_per_turn)
        latch(runtime, OS64_JS_LIMIT, OS64_JS_LIMIT_JOBS, 0);
    if (!runtime->hosted && !observe(runtime) && !pending && runtime->turn &&
        (!runtime->task || runtime->checkpointing)) {
        if (out->status == OS64_JS_OK && runtime->rejections != NULL) {
            out->status = OS64_JS_UNHANDLED_REJECTION;
            rejection_diagnostic(runtime, runtime->rejections->reason, out);
        }
        pending = JS_IsJobPending(runtime->engine);
        if (!pending) clear_rejections(runtime);
    }
    if (observe(runtime)) sticky(runtime, out);
    out->jobs_pending = pending;
    out->jobs_executed = runtime->jobs;
    out->diagnostic_truncated |= runtime->source_truncated |
        copy_text(out->source_name, sizeof(out->source_name), runtime->source_name);
    if ((!pending && !runtime->task) || runtime->failure != OS64_JS_OK) runtime->turn = false;
    runtime->active_outcome = NULL;
    runtime->active = false;
    return out->status;
}

JS_PUBLIC os64_js_status_t os64_js_create_with_teardown(const os64_js_config_t *config,
                                         os64_js_teardown_policy_t policy,
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
    if (policy != OS64_JS_TEARDOWN_FATAL && policy != OS64_JS_TEARDOWN_RECLAIM)
        return status(outcome, OS64_JS_BAD_ARGUMENT, "unknown teardown policy");
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
    runtime->allocator.tracked = policy == OS64_JS_TEARDOWN_RECLAIM;
    runtime->allocator.allocation_limit = storage < limits->memory_bytes
        ? limits->memory_bytes - storage : 0;
    if (storage < limits->memory_bytes)
        runtime->engine = JS_NewRuntime2(runtime->allocator.tracked
            ? &jsport_tracked_malloc_functions : &jsport_malloc_functions, &runtime->allocator);
    if (runtime->engine != NULL) {
        JS_SetMemoryLimit(runtime->engine, runtime->allocator.allocation_limit);
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
        /* Construction owns no caller handles; leftovers here are an invariant
         * failure, not the binding-leak exception selected for normal destroy. */
        if (runtime->allocator.blocks != NULL)
            jsport_fatal("construction allocator ledger", __FILE__, __LINE__);
        os64_free(runtime);
        if (limited) outcome->limit = OS64_JS_LIMIT_MEMORY;
        return status(outcome, limited ? OS64_JS_LIMIT : OS64_JS_HOST_FAILURE,
                      limited ? "runtime construction exceeds memory limit" : "runtime construction failed");
    }
    JS_SetContextOpaque(runtime->context, runtime);
    JS_SetInterruptHandler(runtime->engine, interrupt, runtime);
    JS_SetHostPromiseRejectionTracker(runtime->engine, rejection, runtime);
    JS_SetModuleLoaderFunc(runtime->engine, module_normalize, module_load, runtime);
    *out = runtime;
    return OS64_JS_OK;
}

JS_PUBLIC os64_js_status_t os64_js_create(const os64_js_config_t *config,
                                         const char *caller_abi,
                                         os64_js_runtime_t **out,
                                         os64_js_outcome_t *outcome)
{
    return os64_js_create_with_teardown(config, OS64_JS_TEARDOWN_FATAL, caller_abi, out, outcome);
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

JS_PUBLIC os64_js_status_t os64_js_check_budget(os64_js_runtime_t *runtime,
                                               const char *caller_abi,
                                               os64_js_outcome_t *outcome)
{
    if (outcome == NULL) return OS64_JS_BAD_ARGUMENT;
    if (runtime != NULL && runtime->active_outcome == outcome) return OS64_JS_BUSY;
    os64_memset(outcome, 0, sizeof(*outcome));
    if (runtime == NULL || caller_abi == NULL)
        return status(outcome, OS64_JS_BAD_ARGUMENT, "runtime and ABI are required");
    if (os64_strcmp(caller_abi, OS64_JS_ABI_ID) != 0)
        return status(outcome, OS64_JS_ABI_MISMATCH, "binding header ABI mismatch");
    if (!runtime->active || !runtime->turn)
        return status(outcome, OS64_JS_BAD_ARGUMENT, "an active budgeted callback is required");
    if (!observe(runtime)) return OS64_JS_OK;
    outcome->limit = runtime->failed_limit;
    outcome->host_error = runtime->host_error;
    return status(outcome, runtime->failure, "native work exceeded the active runtime budget");
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

JS_PUBLIC bool os64_js_turn_open(const os64_js_runtime_t *runtime)
{
    return runtime != NULL && (runtime->active || runtime->turn);
}

/* ── Bytecode blobs (BYTECODE_CACHE.md § 4) ─────────────────────────────
 *
 * A blob is a fixed head and the engine's bytes:
 *
 *     "os64-jsbc 1\n"     magic, 12 bytes
 *     engine identity     OS64_JS_ENGINE_ID, 64 hex digits
 *     kind                'c', a classic script; three zero bytes after
 *     payload length      8 bytes, little-endian
 *     payload digest      FNV-1a 64 of the payload, 8 bytes, little-endian
 *
 * JS_ReadObject trusts what it reads, so nothing reaches it that this head
 * does not describe exactly: the identity says the engine that wrote it is
 * this one (a patch can change what the compiler emits without changing
 * upstream's BC_VERSION), and the digest that the bytes are the bytes it
 * wrote. */
#define BLOB_MAGIC "os64-jsbc 1\n"
#define BLOB_MAGIC_LENGTH 12
#define BLOB_ENGINE_LENGTH 64
#define BLOB_KIND_CLASSIC 'c'
#define BLOB_HEAD (BLOB_MAGIC_LENGTH + BLOB_ENGINE_LENGTH + 4 + 8 + 8)

_Static_assert(sizeof(BLOB_MAGIC) - 1 == BLOB_MAGIC_LENGTH, "blob magic length");
_Static_assert(sizeof(OS64_JS_ENGINE_ID) - 1 == BLOB_ENGINE_LENGTH, "engine identity length");

static uint64_t blob_digest(const uint8_t *bytes, size_t length)
{
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    for (size_t i = 0; i < length; i++) {
        h ^= bytes[i];
        h *= UINT64_C(0x100000001b3);
    }
    return h;
}

static void put_u64(uint8_t *at, uint64_t v)
{
    for (int i = 0; i < 8; i++) at[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t get_u64(const uint8_t *at)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)at[i] << (8 * i);
    return v;
}

/* The payload of a blob this engine wrote, or NULL. */
static const uint8_t *blob_payload(const uint8_t *blob, size_t length, size_t *payload_length)
{
    if (blob == NULL || length < BLOB_HEAD ||
        os64_memcmp(blob, BLOB_MAGIC, BLOB_MAGIC_LENGTH) != 0 ||
        os64_memcmp(blob + BLOB_MAGIC_LENGTH, OS64_JS_ENGINE_ID, BLOB_ENGINE_LENGTH) != 0)
        return NULL;
    const uint8_t *kind = blob + BLOB_MAGIC_LENGTH + BLOB_ENGINE_LENGTH;
    if (kind[0] != BLOB_KIND_CLASSIC || kind[1] != 0 || kind[2] != 0 || kind[3] != 0)
        return NULL;
    uint64_t said = get_u64(kind + 4);
    if (said != length - BLOB_HEAD) return NULL;
    const uint8_t *payload = blob + BLOB_HEAD;
    if (get_u64(kind + 12) != blob_digest(payload, (size_t)said)) return NULL;
    *payload_length = (size_t)said;
    return payload;
}

/* The compiled script `function` as a blob, into the bytecode's `kept`.
 * Keeping is the host's convenience, never the script's work: a write that
 * runs out of the runtime's memory leaves nothing, and its refusal is taken
 * back off the allocator's sticky flags, since nothing of the engine's
 * state was half made (the writer frees all it took on failure).
 * The writer is judged by the allocator, not by its answer: upstream's
 * returns what it wrote whatever a failed growth of its buffer or a failed
 * atom index left out of it, a payload that reads back as a broken function
 * (yonder's round trip, under a cut allocation, crashed running one). So a
 * write the allocator refused anything during is no blob. */
static void blob_keep(os64_js_runtime_t *runtime, JSValueConst function, os64_js_bytecode_t *bytecode)
{
    unsigned failures = runtime->allocator.failures;
    size_t size = 0;
    uint8_t *payload = JS_WriteObject(runtime->context, &size, function, JS_WRITE_OBJ_BYTECODE);
    if (payload == NULL || runtime->allocator.failures != failures) {
        if (payload != NULL) js_free(runtime->context, payload);
        discard_exception(runtime->context);
        runtime->allocator.failures = failures;
        return;
    }
    uint8_t *blob = size <= SIZE_MAX - BLOB_HEAD ? os64_malloc(BLOB_HEAD + size) : NULL;
    if (blob != NULL) {
        os64_memcpy(blob, BLOB_MAGIC, BLOB_MAGIC_LENGTH);
        os64_memcpy(blob + BLOB_MAGIC_LENGTH, OS64_JS_ENGINE_ID, BLOB_ENGINE_LENGTH);
        uint8_t *kind = blob + BLOB_MAGIC_LENGTH + BLOB_ENGINE_LENGTH;
        kind[0] = BLOB_KIND_CLASSIC;
        kind[1] = kind[2] = kind[3] = 0;
        put_u64(kind + 4, size);
        put_u64(kind + 12, blob_digest(payload, size));
        os64_memcpy(blob + BLOB_HEAD, payload, size);
        bytecode->kept = blob;
        bytecode->kept_length = BLOB_HEAD + size;
    }
    js_free(runtime->context, payload);
}

/* The script to run: the blob's, read under `source_name`, when it is this
 * engine's and whole; else the source compiled, its blob kept when asked.
 * A read the engine refuses is a miss like any other, unless the runtime
 * failed under it (its memory, its deadline), which is sticky as a
 * compile's failure would be. Unlike the writer (blob_keep), the reader
 * answers every allocation it is refused with an exception: the suite's
 * read sweep refuses each in turn, and none comes back a function. */
static JSValue prepare(os64_js_runtime_t *runtime, const char *source, size_t length,
                       const char *source_name, os64_js_bytecode_t *bytecode)
{
    size_t payload_length = 0;
    const uint8_t *payload = blob_payload(bytecode->blob, bytecode->blob_length, &payload_length);
    if (payload != NULL) {
        JSValue function = JS_ReadObjectNamed(runtime->context, payload, payload_length,
                                              JS_READ_OBJ_BYTECODE, source_name);
        if (JS_VALUE_GET_TAG(function) == JS_TAG_FUNCTION_BYTECODE) {
            bytecode->from_blob = true;
            return function;
        }
        if (JS_IsException(function)) {
            if (observe(runtime)) return function;
            discard_exception(runtime->context);
        } else {
            JS_FreeValue(runtime->context, function);
        }
    }
    JSValue function = JS_Eval(runtime->context, source, length, source_name,
                               JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (!JS_IsException(function) && bytecode->keep) blob_keep(runtime, function, bytecode);
    return function;
}

#ifdef OS64_JS_BYTECODE_ROUND_TRIP
/* The test build's oracle (BYTECODE_CACHE.md § 8), never a product build:
 * every classic evaluation is compiled under a name of its own, written,
 * read back under its real name and run from what was read, so a host
 * suite run in this build proves everything it proves through the blob.
 * A compile that throws is compiled again under its real name, for the
 * error the suite expects, and so is one whose blob could not be had for
 * memory (a suite's injected failure lands on the trip's own allocations);
 * a blob made and then not read back is fatal. */
size_t os64_js_round_trips, os64_js_round_trips_skipped;

static JSValue round_trip(os64_js_runtime_t *runtime, const char *source, size_t length,
                          const char *source_name)
{
    os64_js_bytecode_t written = {.keep = true};
    JSValue function = prepare(runtime, source, length, "os64-round-trip", &written);
    if (JS_IsException(function)) {
        if (observe(runtime)) return function;
        discard_exception(runtime->context);
        return JS_Eval(runtime->context, source, length, source_name,
                       JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    }
    JS_FreeValue(runtime->context, function);
    if (written.kept == NULL) {
        if (observe(runtime)) return JS_EXCEPTION;
        os64_js_round_trips_skipped++;
        return JS_Eval(runtime->context, source, length, source_name,
                       JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    }
    os64_js_bytecode_t read = {.blob = written.kept, .blob_length = written.kept_length};
    function = prepare(runtime, source, length, source_name, &read);
    os64_free(written.kept);
    if (!JS_IsException(function) && !read.from_blob)
        jsport_fatal("bytecode round trip: a blob was not read back", __FILE__, __LINE__);
    os64_js_round_trips += read.from_blob;
    return function;
}
#endif

/* Both buffer and file entry points own an accounted, terminated source here.
 * Loading and source copying finish before the execution deadline starts; a
 * blob's read and a compile are inside it, as the compile always was. */
static os64_js_status_t evaluate(os64_js_runtime_t *runtime, const char *source,
                                size_t length, const char *source_name,
                                os64_js_bytecode_t *bytecode, os64_js_outcome_t *outcome)
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
        JSValue value;
#ifdef OS64_JS_BYTECODE_ROUND_TRIP
        if (bytecode == NULL) {
            value = round_trip(runtime, source, length, source_name);
            if (!JS_IsException(value)) value = JS_EvalFunction(runtime->context, value);
        } else {
#else
        if (bytecode == NULL) {
            value = JS_Eval(runtime->context, source, length, source_name, JS_EVAL_TYPE_GLOBAL);
        } else {
#endif
            value = prepare(runtime, source, length, source_name, bytecode);
            if (!JS_IsException(value)) value = JS_EvalFunction(runtime->context, value);
        }
        if (JS_IsException(value)) {
            JSValue error = JS_GetException(runtime->context);
            caught(runtime, runtime->context, error);
            outcome->status = OS64_JS_EXCEPTION;
            diagnostic(runtime, error, outcome);
            JS_FreeValue(runtime->context, error);
        }
        JS_FreeValue(runtime->context, value);
    }
    return OS64_JS_OK;
}

static os64_js_status_t eval_text(os64_js_runtime_t *runtime, const void *source, size_t length,
                                  const char *source_name, os64_js_bytecode_t *bytecode,
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
    result = evaluate(runtime, copy, length, source_name, bytecode, outcome);
    js_free_rt(runtime->engine, copy);
    if (result == OS64_JS_BAD_ARGUMENT)
        return refuse(runtime, outcome, result, "deadline is not representable");
    return leave(runtime, outcome);
}

JS_PUBLIC os64_js_status_t os64_js_eval(os64_js_runtime_t *runtime,
                                      const void *source, size_t length,
                                      const char *source_name,
                                      os64_js_outcome_t *outcome)
{
    return eval_text(runtime, source, length, source_name, NULL, outcome);
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
    if (runtime->task)
        return refuse(runtime, outcome, OS64_JS_BUSY, "a task is open; its checkpoints drain jobs");
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
            caught(runtime, context, error);
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

JS_PUBLIC os64_js_status_t os64_js_run_bytecode(os64_js_runtime_t *runtime,
                                              const void *source, size_t length,
                                              const char *source_name,
                                              os64_js_bytecode_t *bytecode,
                                              os64_js_outcome_t *outcome)
{
    if (bytecode == NULL) {
        os64_js_status_t result = enter(runtime, outcome);
        if (result != OS64_JS_OK) return result;
        return refuse(runtime, outcome, OS64_JS_BAD_ARGUMENT, "bytecode is required");
    }
    bytecode->kept = NULL;
    bytecode->kept_length = 0;
    bytecode->from_blob = false;
    return complete_run(runtime, eval_text(runtime, source, length, source_name, bytecode, outcome),
                        outcome);
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
            /* The current buffer can terminate an exact-boundary file. Probe
             * for more input before charging a larger allocation. */
            char extra;
            int64_t n = os64_read((int32_t)opened, &extra, 1);
            if (n < 0) { latch(runtime, OS64_JS_HOST_FAILURE, OS64_JS_LIMIT_NONE, n); break; }
            if (n == 0) break;
            if (n != 1) {
                latch(runtime, OS64_JS_HOST_FAILURE, OS64_JS_LIMIT_NONE, 0);
                break;
            }
            if (observe(runtime)) break;
            size_t ceiling = runtime->limits.source_bytes + 1;
            size_t next = capacity > ceiling / 2 ? ceiling : capacity * 2;
            char *grown = js_realloc_rt(runtime->engine, source, next);
            if (grown == NULL) break;
            source = grown;
            capacity = next;
            source[length++] = extra;
            continue;
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
        result = evaluate(runtime, source, length, path, NULL, outcome);
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

/* The task entries share enter()'s guard, and the ABI check that the engine
 * values crossing os64_js_call need. */
static os64_js_status_t enter_task(os64_js_runtime_t *runtime, const char *caller_abi,
                                   bool open, os64_js_outcome_t *outcome)
{
    os64_js_status_t result = enter(runtime, outcome);
    if (result != OS64_JS_OK) return result;
    if (caller_abi == NULL)
        return refuse(runtime, outcome, OS64_JS_BAD_ARGUMENT, "ABI is required");
    if (os64_strcmp(caller_abi, OS64_JS_ABI_ID) != 0)
        return refuse(runtime, outcome, OS64_JS_ABI_MISMATCH, "binding header ABI mismatch");
    if (runtime->task != open)
        return refuse(runtime, outcome, open ? OS64_JS_BAD_ARGUMENT : OS64_JS_BUSY,
                      open ? "no task is open" : "a task is already open");
    return OS64_JS_OK;
}

JS_PUBLIC os64_js_status_t os64_js_task_begin(os64_js_runtime_t *runtime, const char *caller_abi,
                                            const char *source_name, os64_js_outcome_t *outcome)
{
    os64_js_status_t result = enter_task(runtime, caller_abi, false, outcome);
    if (result != OS64_JS_OK) return result;
    if (source_name == NULL)
        return refuse(runtime, outcome, OS64_JS_BAD_ARGUMENT, "task name is required");
    if (runtime->turn || JS_IsJobPending(runtime->engine))
        return refuse(runtime, outcome, OS64_JS_BUSY, "the previous turn has queued jobs");
    if (observe(runtime)) return leave(runtime, outcome);
    int64_t now = os64_micros();
    if (now < 0) {
        latch(runtime, OS64_JS_HOST_FAILURE, OS64_JS_LIMIT_NONE, now);
        return leave(runtime, outcome);
    }
    if (runtime->limits.execution_ms * 1000 > (uint64_t)(INT64_MAX - now))
        return refuse(runtime, outcome, OS64_JS_BAD_ARGUMENT, "deadline is not representable");
    runtime->jobs = 0;
    runtime->source_truncated = copy_text(runtime->source_name, sizeof(runtime->source_name), source_name);
    runtime->deadline = now + (int64_t)(runtime->limits.execution_ms * 1000);
    runtime->turn = true;
    runtime->task = true;
    runtime->evaluated = true;
    return leave(runtime, outcome);
}

JS_PUBLIC os64_js_status_t os64_js_call(os64_js_runtime_t *runtime, const char *caller_abi,
                                      JSValue function, JSValue this_value, int argc,
                                      JSValue *argv, bool *returned_false,
                                      os64_js_outcome_t *outcome)
{
    if (returned_false != NULL) *returned_false = false;
    os64_js_status_t result = enter_task(runtime, caller_abi, true, outcome);
    if (result != OS64_JS_OK) return result;
    if (argc < 0 || (argc != 0 && argv == NULL))
        return refuse(runtime, outcome, OS64_JS_BAD_ARGUMENT, "argument array is invalid");
    if (observe(runtime)) return leave(runtime, outcome);
    JSValue value = JS_Call(runtime->context, function, this_value, argc, argv);
    if (JS_IsException(value)) {
        JSValue error = JS_GetException(runtime->context);
        caught(runtime, runtime->context, error);
        outcome->status = OS64_JS_EXCEPTION;
        diagnostic(runtime, error, outcome);
        JS_FreeValue(runtime->context, error);
    } else if (returned_false != NULL && JS_VALUE_GET_TAG(value) == JS_TAG_BOOL &&
               !JS_VALUE_GET_BOOL(value)) {
        *returned_false = true;
    }
    JS_FreeValue(runtime->context, value);
    return leave(runtime, outcome);
}

/* A checkpoint drains to exhaustion. A job that throws is reported (the first
 * one is the outcome's) and the checkpoint carries on, so a task never ends
 * with runnable jobs it could not run. */
static void checkpoint(os64_js_runtime_t *runtime, os64_js_outcome_t *outcome)
{
    while (JS_IsJobPending(runtime->engine) && !observe(runtime)) {
        if (runtime->jobs == runtime->limits.jobs_per_turn) break;
        runtime->jobs++;
        JSContext *context = NULL;
        if (JS_ExecutePendingJob(runtime->engine, &context) < 0) {
            if (context == NULL) context = runtime->context;
            JSValue error = JS_GetException(context);
            caught(runtime, context, error);
            if (outcome->status == OS64_JS_OK) {
                outcome->status = OS64_JS_EXCEPTION;
                diagnostic(runtime, error, outcome);
            }
            JS_FreeValue(context, error);
        }
    }
    runtime->checkpointing = true;
}

JS_PUBLIC os64_js_status_t os64_js_checkpoint(os64_js_runtime_t *runtime, const char *caller_abi,
                                            os64_js_outcome_t *outcome)
{
    os64_js_status_t result = enter_task(runtime, caller_abi, true, outcome);
    if (result != OS64_JS_OK) return result;
    checkpoint(runtime, outcome);
    result = leave(runtime, outcome);
    runtime->checkpointing = false;
    return result;
}

JS_PUBLIC os64_js_status_t os64_js_task_end(os64_js_runtime_t *runtime, const char *caller_abi,
                                          os64_js_outcome_t *outcome)
{
    if (outcome == NULL) return OS64_JS_BAD_ARGUMENT;
    if (runtime != NULL && runtime->active_outcome == outcome) return OS64_JS_BUSY;
    /* A sticky failure ended the task's work, not the task: closing it is
     * what lets the host see one verdict and go on to drain and destroy. */
    if (runtime != NULL && !runtime->active && runtime->task && runtime->failure != OS64_JS_OK) {
        os64_memset(outcome, 0, sizeof(*outcome));
        runtime->task = false;
        runtime->turn = false;
        sticky(runtime, outcome);
        outcome->jobs_executed = runtime->jobs;
        copy_text(outcome->source_name, sizeof(outcome->source_name), runtime->source_name);
        return outcome->status;
    }
    os64_js_status_t result = enter_task(runtime, caller_abi, true, outcome);
    if (result != OS64_JS_OK) return result;
    checkpoint(runtime, outcome);
    runtime->task = false;
    result = leave(runtime, outcome);
    runtime->checkpointing = false;
    return result;
}

JS_PUBLIC os64_js_status_t os64_js_set_execution_ms(os64_js_runtime_t *runtime, uint64_t ms,
                                                  os64_js_outcome_t *outcome)
{
    os64_js_status_t result = enter(runtime, outcome);
    if (result != OS64_JS_OK) return result;
    if (ms == 0 || ms > INT64_MAX / 1000)
        return refuse(runtime, outcome, OS64_JS_BAD_ARGUMENT, "limits must be positive and representable");
    runtime->limits.execution_ms = ms;
    return refuse(runtime, outcome, OS64_JS_OK, NULL);
}

/* Not through enter(): a host clears itself from a runtime that has failed
 * as readily as from one that has not, before its opaque goes away. */
JS_PUBLIC os64_js_status_t os64_js_set_script_host(os64_js_runtime_t *runtime, const char *caller_abi,
                                                 const os64_js_script_host_t *host,
                                                 os64_js_outcome_t *outcome)
{
    if (outcome == NULL) return OS64_JS_BAD_ARGUMENT;
    if (runtime != NULL && runtime->active_outcome == outcome) return OS64_JS_BUSY;
    os64_memset(outcome, 0, sizeof(*outcome));
    if (runtime == NULL || caller_abi == NULL)
        return status(outcome, OS64_JS_BAD_ARGUMENT, "runtime and ABI are required");
    if (os64_strcmp(caller_abi, OS64_JS_ABI_ID) != 0)
        return status(outcome, OS64_JS_ABI_MISMATCH, "binding header ABI mismatch");
    if (runtime->active) return status(outcome, OS64_JS_BUSY, "runtime is executing");
    runtime->hosted = host != NULL;
    if (host != NULL) runtime->host = *host;
    else os64_memset(&runtime->host, 0, sizeof(runtime->host));
    return OS64_JS_OK;
}

JS_PUBLIC bool os64_js_take_rejection(os64_js_runtime_t *runtime, const char *caller_abi,
                                     JSValue *promise, JSValue *reason, uint32_t *tag)
{
    if (promise != NULL) *promise = JS_UNDEFINED;
    if (reason != NULL) *reason = JS_UNDEFINED;
    if (tag != NULL) *tag = 0;
    if (runtime == NULL || caller_abi == NULL || promise == NULL || reason == NULL ||
        os64_strcmp(caller_abi, OS64_JS_ABI_ID) != 0 || !runtime->hosted || runtime->active ||
        runtime->failure != OS64_JS_OK || runtime->rejections == NULL || JS_IsJobPending(runtime->engine))
        return false;
    Rejection *item = runtime->rejections;
    rejection_unlink(runtime, rejection_place(runtime, JS_VALUE_GET_PTR(item->promise)));
    *promise = item->promise;
    *reason = item->reason;
    if (tag != NULL) *tag = item->tag;
    js_free_rt(runtime->engine, item);
    return true;
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

/* Each value by QuickJS string conversion, spaces between them. */
static bool write_values(os64_js_runtime_t *runtime, JSContext *context, int argc,
                         JSValueConst *args, bool *thrown)
{
    for (int i = 0; i < argc && !observe(runtime); i++) {
        size_t length;
        const char *text = JS_ToCStringLen(context, &length, args[i]);
        if (text == NULL) {
            *thrown = true;
            return false;
        }
        bool written = (i == 0 || write_output(runtime, " ", 1)) &&
                       write_output(runtime, text, length);
        JS_FreeCString(context, text);
        if (!written) return false;
    }
    return !observe(runtime);
}

static JSValue output_ready(os64_js_runtime_t *runtime, JSContext *context)
{
    if (!runtime->active || !runtime->turn)
        jsport_fatal("output outside a runtime turn", __FILE__, __LINE__);
    /* Property traps can see a staged function before setup commits. It must
     * not use a borrowed handle until the entire installation succeeds. */
    if (!runtime->output_installed)
        return JS_ThrowTypeError(context, "output installation is incomplete");
    return JS_UNDEFINED;
}

static JSValue output_call(JSContext *context, JSValueConst self, int argc,
                           JSValueConst *args)
{
    (void)self;
    os64_js_runtime_t *runtime = JS_GetContextOpaque(context);
    JSValue ready = output_ready(runtime, context);
    if (JS_IsException(ready)) return ready;
    bool thrown = false;
    if (write_values(runtime, context, argc, args, &thrown) && write_output(runtime, "\n", 1))
        return JS_UNDEFINED;
    return thrown ? JS_EXCEPTION : JS_ThrowInternalError(context, "host output failed");
}

/* THE CONSOLE NAMESPACE (the WHATWG Console Standard): every method writes
 * lines as print does, indented two spaces per open group. The levels (info,
 * warn, error...) are all one stream here, so they print as log does. The
 * counts and timers are kept in the methods' function data, the group depth
 * on the runtime. */
enum {
    CONSOLE_LOG, CONSOLE_ASSERT, CONSOLE_CLEAR, CONSOLE_GROUP, CONSOLE_GROUP_END,
    CONSOLE_COUNT, CONSOLE_COUNT_RESET, CONSOLE_TIME, CONSOLE_TIME_LOG, CONSOLE_TIME_END
};

static const struct { const char *name; int kind; } console_methods[] = {
    {"log", CONSOLE_LOG}, {"info", CONSOLE_LOG}, {"debug", CONSOLE_LOG},
    {"warn", CONSOLE_LOG}, {"error", CONSOLE_LOG}, {"trace", CONSOLE_LOG},
    {"dir", CONSOLE_LOG}, {"dirxml", CONSOLE_LOG}, {"table", CONSOLE_LOG},
    {"assert", CONSOLE_ASSERT}, {"clear", CONSOLE_CLEAR},
    {"group", CONSOLE_GROUP}, {"groupCollapsed", CONSOLE_GROUP}, {"groupEnd", CONSOLE_GROUP_END},
    {"count", CONSOLE_COUNT}, {"countReset", CONSOLE_COUNT_RESET},
    {"time", CONSOLE_TIME}, {"timeLog", CONSOLE_TIME_LOG}, {"timeEnd", CONSOLE_TIME_END},
};
#define CONSOLE_METHODS (sizeof(console_methods) / sizeof(console_methods[0]))
/* Deeper groups still count; their lines stop moving right. */
#define CONSOLE_INDENT_MAX 32

/* One console line: the indent, the `head` pieces run together, then the
 * values (a space between the head and them). */
static JSValue console_line(os64_js_runtime_t *runtime, JSContext *context,
                            const char *const *head, size_t pieces, int argc, JSValueConst *args)
{
    static const char spaces[2 * CONSOLE_INDENT_MAX] =
        "                                                                ";
    uint32_t depth = runtime->console_depth < CONSOLE_INDENT_MAX ? runtime->console_depth
                                                                 : CONSOLE_INDENT_MAX;
    bool thrown = false;
    bool written = write_output(runtime, spaces, 2 * (size_t)depth);
    for (size_t i = 0; written && i < pieces; i++)
        written = write_output(runtime, head[i], os64_strlen(head[i]));
    written = written && (pieces == 0 || argc == 0 || write_output(runtime, " ", 1)) &&
              write_values(runtime, context, argc, args, &thrown) &&
              write_output(runtime, "\n", 1);
    if (written) return JS_UNDEFINED;
    return thrown ? JS_EXCEPTION : JS_ThrowInternalError(context, "host output failed");
}

/* A count's or a timer's label: its first argument, or "default". */
static JSAtom console_label(JSContext *context, int argc, JSValueConst *args)
{
    if (argc == 0 || JS_IsUndefined(args[0])) return JS_NewAtom(context, "default");
    return JS_ValueToAtom(context, args[0]);
}

/* `<label>: <text>`, or the standard's warning `<before> '<label>' <after>`. */
static JSValue console_labelled(os64_js_runtime_t *runtime, JSContext *context, JSAtom label,
                                const char *before, const char *after, int argc, JSValueConst *args)
{
    const char *name = JS_AtomToCString(context, label);
    if (name == NULL) return JS_EXCEPTION;
    const char *said[] = {name, ": ", after};
    const char *warned[] = {before, " '", name, "' ", after};
    JSValue result = before == NULL ? console_line(runtime, context, said, 3, argc, args)
                                    : console_line(runtime, context, warned, 5, argc, args);
    JS_FreeCString(context, name);
    return result;
}

static JSValue console_call(JSContext *context, JSValueConst self, int argc,
                            JSValueConst *args, int kind, JSValue *data)
{
    (void)self;
    os64_js_runtime_t *runtime = JS_GetContextOpaque(context);
    JSValue ready = output_ready(runtime, context);
    if (JS_IsException(ready)) return ready;
    switch (kind) {
    case CONSOLE_LOG:
        return console_line(runtime, context, NULL, 0, argc, args);
    case CONSOLE_ASSERT: {
        if (argc > 0 && JS_ToBool(context, args[0])) return JS_UNDEFINED;
        /* A message that is a string ends the sentence; any other is a value. */
        if (argc > 1 && JS_IsString(args[1])) {
            const char *first = JS_ToCString(context, args[1]);
            if (first == NULL) return JS_EXCEPTION;
            const char *head[] = {"Assertion failed: ", first};
            JSValue result = console_line(runtime, context, head, 2, argc - 2, args + 2);
            JS_FreeCString(context, first);
            return result;
        }
        const char *head[] = {"Assertion failed"};
        return console_line(runtime, context, head, 1, argc > 1 ? argc - 1 : 0, args + 1);
    }
    case CONSOLE_CLEAR:
        /* A stream cannot be cleared; the groups it was in can. */
        runtime->console_depth = 0;
        return JS_UNDEFINED;
    case CONSOLE_GROUP: {
        JSValue result = argc > 0 ? console_line(runtime, context, NULL, 0, argc, args) : JS_UNDEFINED;
        if (!JS_IsException(result) && runtime->console_depth != UINT32_MAX) runtime->console_depth++;
        return result;
    }
    case CONSOLE_GROUP_END:
        if (runtime->console_depth != 0) runtime->console_depth--;
        return JS_UNDEFINED;
    default:
        break;
    }
    /* The counts and timers: data[0] and data[1], each label to a number. */
    bool counting = kind == CONSOLE_COUNT || kind == CONSOLE_COUNT_RESET;
    JSValueConst table = data[counting ? 0 : 1];
    JSAtom label = console_label(context, argc, args);
    if (label == JS_ATOM_NULL) return JS_EXCEPTION;
    JSValue result = JS_UNDEFINED;
    int has = JS_HasProperty(context, table, label);
    if (has < 0) {
        result = JS_EXCEPTION;
    } else if (kind == CONSOLE_COUNT) {
        int64_t count = 0;
        JSValue was = has ? JS_GetProperty(context, table, label) : JS_UNDEFINED;
        if (JS_IsException(was) || (has && JS_ToInt64(context, &count, was) < 0)) result = JS_EXCEPTION;
        JS_FreeValue(context, was);
        char text[24];
        os64_snprintf(text, sizeof(text), "%ld", (long)(count + 1));
        if (!JS_IsException(result) && JS_SetProperty(context, table, label, JS_NewInt64(context, count + 1)) < 0)
            result = JS_EXCEPTION;
        if (!JS_IsException(result))
            result = console_labelled(runtime, context, label, NULL, text, 0, NULL);
    } else if (kind == CONSOLE_COUNT_RESET) {
        if (!has)
            result = console_labelled(runtime, context, label, "Count for", "does not exist", 0, NULL);
        else if (JS_SetProperty(context, table, label, JS_NewInt32(context, 0)) < 0)
            result = JS_EXCEPTION;
    } else if (kind == CONSOLE_TIME) {
        if (has)
            result = console_labelled(runtime, context, label, "Timer", "already exists", 0, NULL);
        else if (JS_SetProperty(context, table, label, JS_NewInt64(context, os64_micros())) < 0)
            result = JS_EXCEPTION;
    } else if (!has) {
        result = console_labelled(runtime, context, label, "Timer", "does not exist", 0, NULL);
    } else {
        int64_t began = 0;
        JSValue was = JS_GetProperty(context, table, label);
        if (JS_IsException(was) || JS_ToInt64(context, &began, was) < 0) result = JS_EXCEPTION;
        JS_FreeValue(context, was);
        int64_t elapsed = os64_micros() - began;
        if (elapsed < 0) elapsed = 0;
        char text[40];
        os64_snprintf(text, sizeof(text), "%ld.%03ldms", (long)(elapsed / 1000), (long)(elapsed % 1000));
        if (!JS_IsException(result) && kind == CONSOLE_TIME_END &&
            JS_DeleteProperty(context, table, label, 0) < 0)
            result = JS_EXCEPTION;
        if (!JS_IsException(result))
            result = console_labelled(runtime, context, label, NULL, text,
                                      kind == CONSOLE_TIME_LOG && argc > 1 ? argc - 1 : 0, args + 1);
    }
    JS_FreeAtom(context, label);
    return result;
}

/* A method named as the standard names it (data functions are born nameless). */
static JSValue console_method(JSContext *context, size_t i, JSValue *tables)
{
    JSValue method = JS_NewCFunctionData(context, console_call, 0, console_methods[i].kind, 2, tables);
    if (JS_IsException(method)) return method;
    if (JS_DefinePropertyValueStr(context, method, "name", JS_NewString(context, console_methods[i].name),
                                  JS_PROP_CONFIGURABLE) < 0) {
        JS_FreeValue(context, method);
        return JS_EXCEPTION;
    }
    return method;
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
    const uint32_t valid = OS64_JS_OUTPUT_PRINT | OS64_JS_OUTPUT_CONSOLE;
    if (handle < 0 || names == 0 || (names & ~valid) != 0)
        return refuse(runtime, outcome, OS64_JS_BAD_ARGUMENT, "output handle and selected names are required");
    result = setup(runtime, runtime->output_installed, outcome, "<output setup>");
    if (result != OS64_JS_OK) return result;
    if (observe(runtime)) return leave(runtime, outcome);
    JSContext *context = runtime->context;
    JSValue global = JS_GetGlobalObject(context), console = JS_UNDEFINED;
    JSPropertyDescriptor console_descriptor = {0};
    OutputProperty properties[CONSOLE_METHODS + 2] = {0};
    size_t count = 0;
    int console_present = 0;
    int installed = 0;
    if (names & OS64_JS_OUTPUT_CONSOLE) {
        JSAtom name = JS_NewAtom(context, "console");
        console_present = name == JS_ATOM_NULL ? -1 : JS_GetOwnProperty(context, &console_descriptor, global, name);
        JS_FreeAtom(context, name);
        if (console_present == 0) console = JS_NewObject(context);
        else if (console_present > 0) {
            if (JS_IsObject(console_descriptor.value)) console = JS_DupValue(context, console_descriptor.value);
            else console = JS_ThrowTypeError(context, "console must be an own data object");
        }
        if (console_present < 0 || JS_IsException(console)) installed = -1;
        /* The counts and the timers every method shares, as function data. */
        JSValue tables[2] = {JS_UNDEFINED, JS_UNDEFINED};
        if (installed >= 0) {
            tables[0] = JS_NewObjectProto(context, JS_NULL);
            tables[1] = JS_NewObjectProto(context, JS_NULL);
            if (JS_IsException(tables[0]) || JS_IsException(tables[1])) installed = -1;
        }
        for (size_t i = 0; installed >= 0 && i < CONSOLE_METHODS; i++)
            installed = prepare_output_property(context, &properties[count++], console,
                          console_methods[i].name,
                          console_method(context, i, tables));
        JS_FreeValue(context, tables[0]);
        JS_FreeValue(context, tables[1]);
    }
    if (installed >= 0 && (names & OS64_JS_OUTPUT_PRINT))
        installed = prepare_output_property(context, &properties[count++], global, "print",
                      JS_NewCFunction(context, output_call, "print", 1));
    if (installed >= 0 && (names & OS64_JS_OUTPUT_CONSOLE) && console_present == 0)
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

/* ── Modules (CONTRACT.md § Modules) ─────────────────────────────────── */

/* import.meta.url: the module's name, which is its URL. */
static int module_meta(JSContext *context, JSModuleDef *m, const char *name)
{
    JSValue meta = JS_GetImportMeta(context, m);
    if (JS_IsException(meta)) return -1;
    int defined = JS_DefinePropertyValueStr(context, meta, "url", JS_NewString(context, name), JS_PROP_C_W_E);
    JS_FreeValue(context, meta);
    return defined < 0 ? -1 : 0;
}

/* The engine's normaliser: the host's resolve, its answer moved into the
 * engine's allocator, which frees it. */
static char *module_normalize(JSContext *context, const char *base, const char *specifier, void *opaque)
{
    os64_js_runtime_t *runtime = opaque;
    char *name = runtime->loader.resolve != NULL
        ? runtime->loader.resolve(runtime->loader.opaque, base, specifier) : NULL;
    if (name == NULL) {
        JS_ThrowTypeError(context, "cannot resolve module specifier '%s'", specifier);
        return NULL;
    }
    size_t length = os64_strlen(name);
    char *copy = js_malloc(context, length + 1);
    if (copy != NULL) os64_memcpy(copy, name, length + 1);
    os64_free(name);
    return copy;
}

/* The engine's loader, for a name no compiled module has: the host's source,
 * compiled (resolving through this same loader), the module left to the
 * engine's list, which keeps it, as quickjs-libc's loader does. */
static JSModuleDef *module_load(JSContext *context, const char *name, void *opaque)
{
    os64_js_runtime_t *runtime = opaque;
    const char *bytes = NULL;
    size_t length = 0;
    if (runtime->loader.source == NULL ||
        !runtime->loader.source(runtime->loader.opaque, name, &bytes, &length) ||
        (bytes == NULL && length != 0)) {
        JS_ThrowReferenceError(context, "could not load module '%s'", name);
        return NULL;
    }
    if (length > runtime->limits.source_bytes) {
        latch(runtime, OS64_JS_LIMIT, OS64_JS_LIMIT_SOURCE, 0);
        JS_ThrowRangeError(context, "module '%s' exceeds the source limit", name);
        return NULL;
    }
    char *copy = js_malloc(context, length + 1);
    if (copy == NULL) return NULL;
    if (length != 0) os64_memcpy(copy, bytes, length);
    copy[length] = 0;
    JSValue value = JS_Eval(context, copy, length, name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    js_free(context, copy);
    if (JS_IsException(value)) return NULL;
    JSModuleDef *m = JS_VALUE_GET_PTR(value);
    int meta = module_meta(context, m, name);
    JS_FreeValue(context, value);
    return meta < 0 ? NULL : m;
}

JS_PUBLIC os64_js_status_t os64_js_set_module_loader(os64_js_runtime_t *runtime, const char *caller_abi,
                                                   const os64_js_module_loader_t *loader,
                                                   os64_js_outcome_t *outcome)
{
    os64_js_status_t result = enter(runtime, outcome);
    if (result != OS64_JS_OK) return result;
    if (caller_abi == NULL || os64_strcmp(caller_abi, OS64_JS_ABI_ID) != 0)
        return refuse(runtime, outcome, caller_abi == NULL ? OS64_JS_BAD_ARGUMENT : OS64_JS_ABI_MISMATCH,
                      "binding header ABI mismatch");
    if (loader != NULL) runtime->loader = *loader;
    else os64_memset(&runtime->loader, 0, sizeof(runtime->loader));
    return refuse(runtime, outcome, OS64_JS_OK, NULL);
}

JS_PUBLIC os64_js_status_t os64_js_module_compile(os64_js_runtime_t *runtime, const char *caller_abi,
                                                const void *source, size_t length, const char *name,
                                                const char *url, os64_js_module_t **module,
                                                os64_js_outcome_t *outcome)
{
    if (module != NULL) *module = NULL;
    os64_js_status_t result = enter(runtime, outcome);
    if (result != OS64_JS_OK) return result;
    if (caller_abi == NULL || os64_strcmp(caller_abi, OS64_JS_ABI_ID) != 0)
        return refuse(runtime, outcome, caller_abi == NULL ? OS64_JS_BAD_ARGUMENT : OS64_JS_ABI_MISMATCH,
                      "binding header ABI mismatch");
    if ((source == NULL && length != 0) || name == NULL || module == NULL)
        return refuse(runtime, outcome, OS64_JS_BAD_ARGUMENT, "source bytes, name and handle are required");
    runtime->source_truncated = copy_text(runtime->source_name, sizeof(runtime->source_name), name);
    if (length > runtime->limits.source_bytes) {
        latch(runtime, OS64_JS_LIMIT, OS64_JS_LIMIT_SOURCE, 0);
        return leave(runtime, outcome);
    }
    if (observe(runtime)) return leave(runtime, outcome);
    struct os64_js_module *handle = js_mallocz_rt(runtime->engine, sizeof(*handle));
    char *copy = handle != NULL ? js_malloc_rt(runtime->engine, length + 1) : NULL;
    if (copy == NULL) {
        js_free_rt(runtime->engine, handle);
        return leave(runtime, outcome);
    }
    if (length != 0) os64_memcpy(copy, source, length);
    copy[length] = 0;
    JSValue value = JS_Eval(runtime->context, copy, length, name,
                            JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY | JS_EVAL_FLAG_NO_RESOLVE);
    js_free_rt(runtime->engine, copy);
    if (!JS_IsException(value) &&
        module_meta(runtime->context, JS_VALUE_GET_PTR(value), url != NULL ? url : name) < 0) {
        JS_FreeValue(runtime->context, value);
        value = JS_EXCEPTION;
    }
    if (JS_IsException(value)) {
        js_free_rt(runtime->engine, handle);
        if (!observe(runtime)) {
            // The compile's own, not the script host's: no task is running
            // to report it, and a browser reports a module's syntax error
            // when the script that imports it runs.
            JSValue error = JS_GetException(runtime->context);
            outcome->status = OS64_JS_EXCEPTION;
            diagnostic(runtime, error, outcome);
            JS_FreeValue(runtime->context, error);
        }
        return leave(runtime, outcome);
    }
    handle->module = value;
    handle->promise = JS_UNDEFINED;
    handle->next = runtime->modules;
    if (runtime->modules != NULL) runtime->modules->prev = handle;
    runtime->modules = handle;
    *module = handle;
    return leave(runtime, outcome);
}

JS_PUBLIC os64_js_status_t os64_js_module_requests(os64_js_runtime_t *runtime, const char *caller_abi,
                                                 os64_js_module_t *module,
                                                 void (*each)(void *opaque, const char *specifier),
                                                 void *opaque, os64_js_outcome_t *outcome)
{
    os64_js_status_t result = enter(runtime, outcome);
    if (result != OS64_JS_OK) return result;
    if (caller_abi == NULL || os64_strcmp(caller_abi, OS64_JS_ABI_ID) != 0)
        return refuse(runtime, outcome, caller_abi == NULL ? OS64_JS_BAD_ARGUMENT : OS64_JS_ABI_MISMATCH,
                      "binding header ABI mismatch");
    if (module == NULL || each == NULL)
        return refuse(runtime, outcome, OS64_JS_BAD_ARGUMENT, "module and callback are required");
    JSModuleDef *m = JS_VALUE_GET_PTR(module->module);
    int count = JS_GetModuleRequestCount(runtime->context, m);
    for (int i = 0; i < count && !observe(runtime); i++) {
        JSAtom request = JS_GetModuleRequest(runtime->context, m, i);
        const char *text = JS_AtomToCString(runtime->context, request);
        JS_FreeAtom(runtime->context, request);
        if (text == NULL) {
            discard_exception(runtime->context);
            break;
        }
        each(opaque, text);
        JS_FreeCString(runtime->context, text);
    }
    return leave(runtime, outcome);
}

JS_PUBLIC os64_js_status_t os64_js_module_run(os64_js_runtime_t *runtime, const char *caller_abi,
                                            os64_js_module_t *module, os64_js_outcome_t *outcome)
{
    os64_js_status_t result = enter_task(runtime, caller_abi, true, outcome);
    if (result != OS64_JS_OK) return result;
    if (module == NULL || module->run)
        return refuse(runtime, outcome, OS64_JS_BAD_ARGUMENT, "a module is run once");
    if (observe(runtime)) return leave(runtime, outcome);
    module->run = true;
    JSValue promise = JS_EXCEPTION;
    if (JS_ResolveModule(runtime->context, module->module) == 0)
        promise = JS_EvalFunction(runtime->context, JS_DupValue(runtime->context, module->module));
    if (JS_IsException(promise)) {
        // Resolving or linking threw: no evaluation, so no promise.
        JSValue error = JS_GetException(runtime->context);
        caught(runtime, runtime->context, error);
        outcome->status = OS64_JS_EXCEPTION;
        diagnostic(runtime, error, outcome);
        module->rejected = true;
        module->failure = *outcome;
        JS_FreeValue(runtime->context, error);
        return leave(runtime, outcome);
    }
    module->promise = promise;
    if (JS_PromiseState(runtime->context, promise) == JS_PROMISE_REJECTED) {
        /* It failed before the handle held its promise, so the tracker took
         * it for an unhandled rejection: it is this module's failure. */
        Rejection **place = rejection_place(runtime, JS_VALUE_GET_PTR(promise));
        if (place != NULL && *place != NULL && *place != &rejection_removed) {
            Rejection *item = *place;
            rejection_unlink(runtime, place);
            JS_FreeValueRT(runtime->engine, item->promise);
            JS_FreeValueRT(runtime->engine, item->reason);
            js_free_rt(runtime->engine, item);
        }
        JSValue reason = JS_PromiseResult(runtime->context, promise);
        module_failed(runtime, runtime->context, module, reason);
        JS_FreeValue(runtime->context, reason);
        *outcome = module->failure;
        outcome->status = OS64_JS_EXCEPTION;
    }
    return leave(runtime, outcome);
}

JS_PUBLIC os64_js_module_state_t os64_js_module_state(os64_js_runtime_t *runtime, os64_js_module_t *module,
                                                    os64_js_outcome_t *failure)
{
    if (failure != NULL) os64_memset(failure, 0, sizeof(*failure));
    if (runtime == NULL || module == NULL || !module->run) return OS64_JS_MODULE_UNRUN;
    if (module->rejected) {
        if (failure != NULL) *failure = module->failure;
        return OS64_JS_MODULE_REJECTED;
    }
    switch (JS_PromiseState(runtime->context, module->promise)) {
    case JS_PROMISE_FULFILLED: return OS64_JS_MODULE_FULFILLED;
    case JS_PROMISE_REJECTED:
        // Rejected with a handler already on it, which no tracker reports.
        if (failure != NULL) {
            failure->status = OS64_JS_EXCEPTION;
            copy_text(failure->message, sizeof(failure->message), "module evaluation failed");
        }
        return OS64_JS_MODULE_REJECTED;
    default: return OS64_JS_MODULE_PENDING;
    }
}

static void module_free(os64_js_runtime_t *runtime, struct os64_js_module *module)
{
    if (module->prev != NULL) module->prev->next = module->next;
    else runtime->modules = module->next;
    if (module->next != NULL) module->next->prev = module->prev;
    JS_FreeValueRT(runtime->engine, module->module);
    JS_FreeValueRT(runtime->engine, module->promise);
    js_free_rt(runtime->engine, module);
}

JS_PUBLIC void os64_js_module_release(os64_js_runtime_t *runtime, os64_js_module_t *module)
{
    if (runtime == NULL || module == NULL) return;
    if (runtime->active) jsport_fatal("module release during an active runtime call", __FILE__, __LINE__);
    module_free(runtime, module);
}

JS_PUBLIC void os64_js_destroy_report(os64_js_runtime_t *runtime,
                                      os64_js_teardown_report_t *report)
{
    if (report != NULL) os64_memset(report, 0, sizeof(*report));
    if (runtime == NULL) return;
    if (runtime->active) jsport_fatal("destroy during an active runtime call", __FILE__, __LINE__);
    runtime->active = true;
    runtime->failure = OS64_JS_FAILED_RUNTIME;
    runtime->hosted = false;
    JS_SetHostPromiseRejectionTracker(runtime->engine, NULL, NULL);
    clear_rejections(runtime);
    // The host's module handles go before the context they belong to.
    while (runtime->modules != NULL)
        module_free(runtime, runtime->modules);
    JS_FreeContext(runtime->context);
    if (runtime->allocator.tracked) {
        bool leaked = jsport_free_runtime_report(runtime->engine) != 0;
        /* A clean engine verdict may still leave raw buffers/strings retained
         * by C. They also belong to this dead runtime's allocator ledger. */
        leaked |= runtime->allocator.blocks != NULL;
        if (report != NULL) {
            report->leaked = leaked;
            report->reclaimed_blocks = runtime->allocator.live_blocks;
            report->reclaimed_bytes = runtime->allocator.live_bytes;
        }
        runtime->engine = NULL;
        runtime->context = NULL;
        jsport_allocator_reclaim(&runtime->allocator);
    } else {
        JS_FreeRuntime(runtime->engine);
    }
    os64_free(runtime);
}

JS_PUBLIC void os64_js_destroy(os64_js_runtime_t *runtime)
{
    os64_js_destroy_report(runtime, NULL);
}
