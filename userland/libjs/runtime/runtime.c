#include "os64/js_engine.h"
#include "os64/mem.h"
#include "os64/io.h"
#include "os64/proc.h"
#include "os64/str.h"
#include "os64/fmt.h"
#include "allocator.h"
#include "platform.h"
#include "os64_js_engine_id.h"
#include "../intl/intl.h"
#include <limits.h>

#define JS_PUBLIC __attribute__((visibility("default")))

/* A rejected promise nothing handles yet: oldest first on a list, and found
 * by its promise in a table, so a page's thousandth rejection or handler
 * costs what its first did. */
typedef struct Rejection {
    struct Rejection *next, *prev;
    JSValue promise, reason;
    uint32_t tag;           // the script host's answer when it was rejected
    /* A stable slot fits beside tag without growing each pending record.
     * Rejection floods must retain the runtime's existing memory bound. */
    uint32_t realm_index;
} Rejection;

/* Realm slots remain stable through engine finalization. Their enclosing
 * wrapper allocation is reserved against the configured memory ceiling. */
struct os64_js_realm {
    os64_js_runtime_t *runtime;
    JSContext *context;
    os64_js_script_host_t host;
    os64_js_module_loader_t loader;
    struct os64_js_module *modules;
    JSValue promise_then;
    bool hosted, host_configured, loader_configured;
    bool evaluated, output_installed, args_installed, intl_installed;
    unsigned installing;
    int32_t output_handle;
    uint32_t console_depth;
};

typedef struct SetupCall {
    struct SetupCall *previous;
    os64_js_realm_t *realm;
    os64_js_outcome_t *outcome;
    bool borrowed, inherited, nested, prepared;
    unsigned facility;
    const char *name;
} SetupCall;
struct os64_js_runtime {
    JSRuntime *engine;
    JSPortAllocator allocator;
    os64_js_limits_t limits;
    os64_js_outcome_t *active_outcome;
    SetupCall *setup_calls;
    os64_js_realm_t realms[OS64_JS_MAX_REALMS];
    size_t realm_count;
    os64_js_realm_t *turn_owner;
    os64_js_rejection_router_t router;
    Rejection *rejections, *rejections_last;
    /* Promise identity is runtime-wide, including handling from a different
     * realm. Removed slots participate in the growth threshold. */
    Rejection **rejection_table;
    size_t rejection_capacity, rejection_count, rejection_used;
    int64_t deadline, host_error;
    uint64_t jobs;
    os64_js_status_t failure;
    os64_js_limit_t failed_limit;
    unsigned cancelled;
    bool active, turn, task, checkpointing, source_truncated, destroying;
    bool ran_jobs;
    uint64_t owed_count;
    char source_name[OS64_JS_SOURCE_NAME_CAP];
};

static os64_js_realm_t *primary(os64_js_runtime_t *runtime)
{
    return runtime != NULL ? &runtime->realms[0] : NULL;
}

static os64_js_realm_t *realm_for_context(os64_js_runtime_t *runtime, JSContext *context)
{
    for (size_t i = 0; i < runtime->realm_count; i++)
        if (runtime->realms[i].context == context) return &runtime->realms[i];
    return NULL;
}

static bool realm_member(os64_js_runtime_t *runtime, const os64_js_realm_t *realm)
{
    for (size_t i = 0; i < runtime->realm_count; i++)
        if (&runtime->realms[i] == realm) return true;
    return false;
}

/* CFunctionData callbacks receive their caller's context in QuickJS. Keep
 * the service owner's stable slot in function data instead of inferring it
 * from that context when a method or promise resolver crosses realms. */
static os64_js_realm_t *data_realm(JSContext *context, JSValue slot)
{
    os64_js_runtime_t *runtime = JS_GetRuntimeOpaque(JS_GetRuntime(context));
    int32_t index = JS_VALUE_GET_INT(slot);
    if (JS_VALUE_GET_TAG(slot) != JS_TAG_INT || index < 0 ||
        (size_t)index >= runtime->realm_count)
        jsport_fatal("native service realm slot", __FILE__, __LINE__);
    return &runtime->realms[index];
}

/* A module the host compiled: a reference of its own to the engine's
 * module, and once run, its evaluation's promise; a rejection of that
 * promise is the module's failure, kept here as its outcome rather than
 * judged as an unhandled rejection (HTML reports it as an error). A failure
 * after its run returned is `owed` (its place among the owed, from 1; 0
 * when nothing is owed): no call of its own carries it, so the call whose
 * jobs surfaced it does (module_owed_fold). */
struct os64_js_module {
    struct os64_js_module *next, *prev;
    JSValue module, promise;
    os64_js_realm_t *realm;
    bool run, rejected;
    uint64_t owed;
    os64_js_outcome_t failure;
};

static bool outcome_in_use(const os64_js_runtime_t *runtime, const os64_js_outcome_t *out)
{
    if (runtime == NULL) return false;
    if (runtime->active_outcome == out) return true;
    for (const SetupCall *c = runtime->setup_calls; c != NULL; c = c->previous)
        if (c->outcome == out) return true;
    return false;
}

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
static void discard_exception(JSContext *context);
static void rejection_diagnostic(os64_js_runtime_t *runtime, JSContext *context, JSValueConst value,
                                 os64_js_outcome_t *out);

/* The handle whose evaluation `promise` is, or NULL. */
static struct os64_js_module *module_of(os64_js_realm_t *realm, JSValueConst promise)
{
    for (struct os64_js_module *m = realm->modules; m != NULL; m = m->next)
        if (m->run && JS_VALUE_GET_TAG(m->promise) == JS_TAG_OBJECT &&
            JS_VALUE_GET_PTR(m->promise) == JS_VALUE_GET_PTR(promise))
            return m;
    return NULL;
}

/* A module's evaluation rejected: its failure, kept for os64_js_module_state
 * under the module's name and reported as an exception (the script host's
 * `exception`), which is what HTML does with a module script's error; one
 * that comes after the run returned is `owed` to a later call's outcome.
 * Runs no script. */
static void module_failed(os64_js_runtime_t *runtime, JSContext *context,
                          struct os64_js_module *module, JSValueConst reason, bool owed)
{
    if (module->rejected) return;
    module->rejected = true;
    module->owed = owed ? ++runtime->owed_count : 0;
    os64_memset(&module->failure, 0, sizeof(module->failure));
    module->failure.status = OS64_JS_EXCEPTION;
    rejection_diagnostic(runtime, context, reason, &module->failure);
    JSAtom name = JS_GetModuleName(context, JS_VALUE_GET_PTR(module->module));
    const char *text = name != JS_ATOM_NULL ? JS_AtomToCString(context, name) : NULL;
    if (text != NULL) {
        module->failure.diagnostic_truncated |=
            copy_text(module->failure.source_name, sizeof(module->failure.source_name), text);
        JS_FreeCString(context, text);
    } else if (name != JS_ATOM_NULL) discard_exception(context);
    JS_FreeAtom(context, name);
    // Its fallback names an unhandled rejection, which this is not.
    if (os64_strcmp(module->failure.message, "unhandled Promise rejection") == 0)
        copy_text(module->failure.message, sizeof(module->failure.message), "module evaluation failed");
    caught(runtime, module->realm->context, reason);
}

/* Promise.prototype.then from a context no script has run in yet; undefined
 * when it cannot be read (no memory). */
static JSValue intrinsic_then(JSContext *context)
{
    JSValue global = JS_GetGlobalObject(context);
    JSValue constructor = JS_GetPropertyStr(context, global, "Promise");
    JSValue prototype = JS_IsException(constructor) ? JS_EXCEPTION
                                                    : JS_GetPropertyStr(context, constructor, "prototype");
    JSValue then = JS_IsException(prototype) ? JS_EXCEPTION : JS_GetPropertyStr(context, prototype, "then");
    JS_FreeValue(context, prototype);
    JS_FreeValue(context, constructor);
    JS_FreeValue(context, global);
    if (JS_IsException(then)) {
        discard_exception(context);
        return JS_UNDEFINED;
    }
    if (!JS_IsFunction(context, then)) {
        JS_FreeValue(context, then);
        return JS_UNDEFINED;
    }
    return then;
}

/* A watched evaluation rejected (module_watch); its promise is the
 * function's data, and a handle released since finds none. */
static JSValue module_rejected(JSContext *context, JSValueConst self, int argc, JSValueConst *argv,
                               int magic, JSValue *data)
{
    (void)self; (void)magic;
    os64_js_realm_t *realm = data_realm(context, data[1]);
    os64_js_runtime_t *runtime = realm->runtime;
    struct os64_js_module *module = module_of(realm, data[0]);
    if (module != NULL) module_failed(runtime, realm->context, module, argc > 0 ? argv[0] : JS_UNDEFINED, true);
    return JS_UNDEFINED;
}

/* A module still evaluating when its run returns is watched with a
 * rejection handler of libjs's own, through the engine's own `then`, so its
 * failure is heard whatever else handles the promise: an import() of the
 * module puts a handler on it too, and then the tracker never hears it.
 * `then` reads the promise's `constructor` and that constructor's species,
 * which a page can replace, and so could run the page's code here, or
 * throw and leave the module unwatched. The evaluation's promise is the
 * engine's own and no script ever holds it (an import() is answered with a
 * promise of its own), so it is given an own `constructor` of undefined
 * first, which makes `then` take the realm's Promise and ask the page
 * nothing. (A refused allocation here ends the runtime, as one anywhere
 * does.) */
static void module_watch(os64_js_realm_t *realm, JSValueConst promise)
{
    JSContext *context = realm->context;
    if (JS_DefinePropertyValueStr(context, promise, "constructor", JS_UNDEFINED, JS_PROP_CONFIGURABLE) < 0) {
        discard_exception(context);
        return;
    }
    JSValue data[2] = {promise, JS_NewInt32(context, (int32_t)(realm - realm->runtime->realms))};
    JSValue heard = JS_NewCFunctionData(context, module_rejected, 1, 0, 2, data);
    if (JS_IsException(heard)) {
        discard_exception(context);
        return;
    }
    JSValueConst args[2] = {JS_UNDEFINED, heard};
    JSValue derived = JS_Call(context, realm->promise_then, promise, 2, args);
    if (JS_IsException(derived)) discard_exception(context);
    JS_FreeValue(context, derived);
    JS_FreeValue(context, heard);
}

/* An owed failure (module_failed) is the outcome of the call whose jobs
 * surfaced it, the first of them while that outcome is still OK; another
 * waits for the next call that runs jobs or checkpoints (never a task's
 * begin, whose outcome decides whether the task runs at all), so each is
 * reported once, under its module's name. The host's exception hook heard
 * it when it failed (a page's window error event comes from there); this
 * is the report a host keeps. */
static void module_owed_fold(os64_js_runtime_t *runtime, os64_js_outcome_t *out)
{
    if (out->status != OS64_JS_OK) return;
    struct os64_js_module *first = NULL;
    for (size_t r = 0; r < runtime->realm_count; r++)
      for (struct os64_js_module *m = runtime->realms[r].modules; m != NULL; m = m->next)
        if (m->owed != 0 && (first == NULL || m->owed < first->owed)) first = m;
    if (first == NULL) return;
    first->owed = 0;
    out->status = OS64_JS_EXCEPTION;
    out->line = first->failure.line;
    out->column = first->failure.column;
    out->diagnostic_truncated |= first->failure.diagnostic_truncated;
    copy_text(out->message, sizeof(out->message), first->failure.message);
    copy_text(out->stack_trace, sizeof(out->stack_trace), first->failure.stack_trace);
    copy_text(out->source_name, sizeof(out->source_name), first->failure.source_name);
}

/* The engine's tracker: a promise rejected with no handler, or a handler
 * added to one. A handler for a promise this table no longer holds, which a
 * host took (os64_js_take_rejection), is the host's to hear of. A module's
 * evaluation never comes here as one: one that fails at once is taken back
 * by os64_js_module_run as the module's failure, and one still pending is
 * watched (module_watch), so it has a handler. */
static void rejection(JSContext *context, JSValueConst promise,
                      JSValueConst reason, JS_BOOL handled, void *opaque)
{
    os64_js_runtime_t *runtime = opaque;
    if (observe(runtime)) return;
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
    os64_js_realm_t *realm = runtime->router.select != NULL
        ? runtime->router.select(runtime->router.opaque, context, promise, handled)
        : realm_for_context(runtime, context);
    if (observe(runtime) || realm == NULL) return;
    if (!realm_member(runtime, realm)) {
        latch(runtime, OS64_JS_HOST_FAILURE, OS64_JS_LIMIT_NONE, 0);
        return;
    }
    if (handled) {
        if (realm->hosted && realm->host.handled != NULL)
            realm->host.handled(realm->host.opaque, context, promise);
        return;
    }
    if (!rejection_room(runtime)) return;
    Rejection *item = js_malloc_rt(runtime->engine, sizeof(*item));
    if (item == NULL) return;
    *item = (Rejection){.realm_index = (uint32_t)(realm - runtime->realms),
                        .promise = JS_DupValue(context, promise),
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
    if (realm->hosted && realm->host.rejected != NULL)
        item->tag = realm->host.rejected(realm->host.opaque, context, promise);
}

/* An exception the wrapper caught, for a host that reports script failures
 * itself; never one a sticky failure threw. */
static void caught(os64_js_runtime_t *runtime, JSContext *context, JSValueConst error)
{
    os64_js_realm_t *realm = realm_for_context(runtime, context);
    if (realm == NULL) jsport_fatal("exception context membership", __FILE__, __LINE__);
    if (realm->hosted && realm->host.exception != NULL && !observe(runtime))
        realm->host.exception(realm->host.opaque, context, error);
}

static void discard_exception(JSContext *context)
{
    JSValue secondary = JS_GetException(context);
    JS_FreeValue(context, secondary);
}

/* Conversion and property access can execute script. Keep the original turn
 * armed and the public entry guard held while extracting diagnostics. */
static void diagnostic(os64_js_runtime_t *runtime, JSContext *context, JSValueConst value,
                       os64_js_outcome_t *out)
{
    copy_text(out->message, sizeof(out->message), "JavaScript exception; diagnostic unavailable");
    if (observe(runtime)) return;
    const char *text = JS_ToCString(context, value);
    if (text != NULL) {
        out->diagnostic_truncated |= copy_text(out->message, sizeof(out->message), text);
        JS_FreeCString(context, text);
    } else {
        discard_exception(context);
    }
    if (!JS_IsObject(value) || observe(runtime)) return;
    JSValue stack = JS_GetPropertyStr(context, value, "stack");
    if (JS_IsException(stack)) {
        discard_exception(context);
    } else if (JS_IsString(stack)) {
        text = JS_ToCString(context, stack);
        if (text != NULL) {
            out->diagnostic_truncated |= copy_text(out->stack_trace, sizeof(out->stack_trace), text);
            JS_FreeCString(context, text);
        } else {
            discard_exception(context);
        }
    }
    JS_FreeValue(context, stack);
}

/* A checkpoint must not create more jobs by calling a rejection reason's
 * toString. Read string data on a real Error, or render a primitive; other
 * objects keep the fixed fallback. Error proxies do not pass JS_IsError. */
static void rejection_diagnostic(os64_js_runtime_t *runtime, JSContext *context, JSValueConst value,
                                 os64_js_outcome_t *out)
{
    copy_text(out->message, sizeof(out->message), "unhandled Promise rejection");
    if (!JS_IsObject(value)) {
        diagnostic(runtime, context, value, out);
        return;
    }
    if (!JS_IsError(context, value) || observe(runtime)) return;
    const char *names[] = {"message", "stack"};
    char *fields[] = {out->message, out->stack_trace};
    const size_t sizes[] = {sizeof(out->message), sizeof(out->stack_trace)};
    for (size_t i = 0; i < 2 && !observe(runtime); i++) {
        JSAtom name = JS_NewAtom(context, names[i]);
        if (name == JS_ATOM_NULL) { discard_exception(context); break; }
        JSPropertyDescriptor descriptor = {0};
        int present = JS_GetOwnProperty(context, &descriptor, value, name);
        JS_FreeAtom(context, name);
        if (present < 0) discard_exception(context);
        if (present > 0) {
            if (JS_IsString(descriptor.value)) {
                const char *text = JS_ToCString(context, descriptor.value);
                if (text != NULL) {
                    out->diagnostic_truncated |= copy_text(fields[i], sizes[i], text);
                    JS_FreeCString(context, text);
                } else discard_exception(context);
            }
            JS_FreeValue(context, descriptor.value);
            JS_FreeValue(context, descriptor.getter);
            JS_FreeValue(context, descriptor.setter);
        }
    }
}

static os64_js_status_t enter(os64_js_runtime_t *runtime, os64_js_outcome_t *out)
{
    if (out == NULL) return OS64_JS_BAD_ARGUMENT;
    /* A callback must use its own outcome. Refuse an alias before clearing
     * storage that belongs to the active outer call. */
    if (outcome_in_use(runtime, out)) return OS64_JS_BUSY;
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
    if (!observe(runtime) && !pending && runtime->turn &&
        (!runtime->task || runtime->checkpointing)) {
        Rejection *item = runtime->rejections;
        while (item != NULL && !observe(runtime)) {
            Rejection *next = item->next;
            os64_js_realm_t *realm = &runtime->realms[item->realm_index];
            if (!realm->hosted) {
                if (out->status == OS64_JS_OK) {
                    out->status = OS64_JS_UNHANDLED_REJECTION;
                    rejection_diagnostic(runtime, realm->context, item->reason, out);
                }
                rejection_unlink(runtime, rejection_place(runtime, JS_VALUE_GET_PTR(item->promise)));
                JS_FreeValueRT(runtime->engine, item->promise);
                JS_FreeValueRT(runtime->engine, item->reason);
                js_free_rt(runtime->engine, item);
            }
            item = next;
        }
        pending = JS_IsJobPending(runtime->engine);
    }
    if (observe(runtime)) sticky(runtime, out);
    out->jobs_pending = pending;
    out->jobs_executed = runtime->jobs;
    out->diagnostic_truncated |= runtime->source_truncated |
        copy_text(out->source_name, sizeof(out->source_name), runtime->source_name);
    if (runtime->ran_jobs || runtime->checkpointing) module_owed_fold(runtime, out);
    runtime->ran_jobs = false;
    if ((!pending && !runtime->task) || runtime->failure != OS64_JS_OK) runtime->turn = false;
    runtime->active_outcome = NULL;
    runtime->active = false;
    return out->status;
}

static bool realm_initialize(os64_js_realm_t *realm)
{
    realm->promise_then = JS_UNDEFINED;
    realm->context = JS_NewContext(realm->runtime->engine);
    if (realm->context == NULL) return false;
    JS_SetContextOpaque(realm->context, realm);
    JSValue global = JS_GetGlobalObject(realm->context);
    JSAtom name = JS_NewAtom(realm->context, "SharedArrayBuffer");
    bool ready = name != JS_ATOM_NULL &&
        JS_DeleteProperty(realm->context, global, name, JS_PROP_THROW) == 1;
    JS_FreeAtom(realm->context, name);
    JS_FreeValue(realm->context, global);
    if (ready) {
        realm->promise_then = intrinsic_then(realm->context);
        ready = JS_IsFunction(realm->context, realm->promise_then);
    }
    return ready;
}

/* Setup can own an idle turn or borrow the task that synchronously creates
 * a child. The stack of setup outcomes protects each caller's storage. */
static os64_js_status_t setup_enter(os64_js_realm_t *realm, os64_js_outcome_t *out,
                                    bool nested, SetupCall *call)
{
    os64_js_runtime_t *runtime = realm != NULL ? realm->runtime : NULL;
    if (out == NULL) return OS64_JS_BAD_ARGUMENT;
    if (outcome_in_use(runtime, out)) return OS64_JS_BUSY;
    os64_memset(out, 0, sizeof(*out));
    if (runtime == NULL) return status(out, OS64_JS_BAD_ARGUMENT, "realm is required");
    if (runtime->destroying || runtime->failure != OS64_JS_OK)
        return status(out, OS64_JS_FAILED_RUNTIME, "runtime has failed");
    bool borrowed = runtime->active;
    if (borrowed && (!nested || !runtime->turn))
        return status(out, OS64_JS_BUSY, "runtime is executing");
    if (nested && !borrowed && !runtime->task &&
        (runtime->turn || JS_IsJobPending(runtime->engine)))
        return status(out, OS64_JS_BUSY, "the previous turn has queued jobs");
    if (!borrowed) {
        os64_js_status_t result = enter(runtime, out);
        if (result != OS64_JS_OK) return result;
    }
    *call = (SetupCall){.previous = runtime->setup_calls, .realm = realm, .outcome = out,
        .borrowed = borrowed, .inherited = runtime->turn, .nested = nested};
    runtime->setup_calls = call;
    return OS64_JS_OK;
}

static os64_js_status_t setup_leave(SetupCall *call)
{
    os64_js_realm_t *realm = call->realm;
    os64_js_runtime_t *runtime = realm->runtime;
    os64_js_outcome_t *out = call->outcome;
    realm->installing &= ~call->facility;
    runtime->setup_calls = call->previous;
    if (call->inherited) {
        if (call->prepared && observe(runtime)) sticky(runtime, out);
        out->jobs_pending = JS_IsJobPending(runtime->engine);
        out->jobs_executed = runtime->jobs;
        if (call->name != NULL)
            out->diagnostic_truncated |= copy_text(out->source_name, sizeof(out->source_name), call->name);
        if (!call->borrowed) {
            runtime->active = false;
            runtime->active_outcome = NULL;
        }
        return out->status;
    }
    return call->prepared ? leave(runtime, out) : refuse(runtime, out, out->status, NULL);
}

static os64_js_status_t setup_refuse(SetupCall *call, os64_js_status_t value, const char *message)
{
    status(call->outcome, value, message);
    return setup_leave(call);
}

static os64_js_status_t setup_prepare(SetupCall *call, bool installed, unsigned facility, const char *name)
{
    os64_js_realm_t *realm = call->realm;
    os64_js_runtime_t *runtime = realm->runtime;
    if ((facility != 0 && (realm->evaluated || installed || (realm->installing & facility))) ||
        (!call->nested && (runtime->turn || JS_IsJobPending(runtime->engine))))
        return setup_refuse(call, OS64_JS_BAD_ARGUMENT, "installer requires unused setup");
    call->name = name;
    call->facility = facility;
    realm->installing |= facility;
    if (!call->inherited) {
        int64_t now = os64_micros();
        if (now < 0) latch(runtime, OS64_JS_HOST_FAILURE, OS64_JS_LIMIT_NONE, now);
        else if (runtime->limits.execution_ms * 1000 > (uint64_t)(INT64_MAX - now))
            return setup_refuse(call, OS64_JS_BAD_ARGUMENT, "deadline is not representable");
        else {
            runtime->deadline = now + (int64_t)(runtime->limits.execution_ms * 1000);
            runtime->turn = true;
            runtime->turn_owner = realm;
            runtime->jobs = 0;
            runtime->source_truncated = copy_text(runtime->source_name, sizeof(runtime->source_name), name);
        }
    }
    call->prepared = true;
    return OS64_JS_OK;
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
    os64_js_realm_t *realm = primary(runtime);
    realm->runtime = runtime;
    realm->promise_then = JS_UNDEFINED;
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
    }
    bool ready = runtime->engine != NULL && realm_initialize(realm);
    if (!ready || runtime->allocator.failures != 0) {
        bool limited = storage >= limits->memory_bytes ||
            (runtime->allocator.failures & JSPORT_ALLOC_LIMIT);
        if (realm->context != NULL) {
            JS_FreeValue(realm->context, realm->promise_then);
            JS_FreeContext(realm->context);
        }
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
    runtime->realm_count = 1;
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
    return os64_js_realm_context(primary(runtime), caller_abi, outcome);
}

JS_PUBLIC JSContext *os64_js_realm_context(os64_js_realm_t *realm, const char *caller_abi,
                                          os64_js_outcome_t *outcome)
{
    os64_js_runtime_t *runtime = realm != NULL ? realm->runtime : NULL;
    if (outcome == NULL || outcome_in_use(runtime, outcome)) return NULL;
    os64_memset(outcome, 0, sizeof(*outcome));
    if (runtime == NULL || caller_abi == NULL) {
        status(outcome, OS64_JS_BAD_ARGUMENT, "realm and ABI are required");
        return NULL;
    }
    if (os64_strcmp(caller_abi, OS64_JS_ABI_ID) != 0) {
        status(outcome, OS64_JS_ABI_MISMATCH, "binding header ABI mismatch");
        return NULL;
    }
    if (runtime->destroying || runtime->failure != OS64_JS_OK ||
        __atomic_load_n(&runtime->cancelled, __ATOMIC_ACQUIRE)) {
        status(outcome, OS64_JS_FAILED_RUNTIME, "runtime has failed");
        return NULL;
    }
    return realm->context;
}

JS_PUBLIC os64_js_realm_t *os64_js_primary_realm(os64_js_runtime_t *runtime, const char *caller_abi,
                                                os64_js_outcome_t *outcome)
{
    return os64_js_context(runtime, caller_abi, outcome) != NULL ? primary(runtime) : NULL;
}

JS_PUBLIC os64_js_realm_t *os64_js_realm_from_context(os64_js_runtime_t *runtime, JSContext *context,
                                                     const char *caller_abi, os64_js_outcome_t *outcome)
{
    if (os64_js_context(runtime, caller_abi, outcome) == NULL) return NULL;
    os64_js_realm_t *realm = realm_for_context(runtime, context);
    if (realm == NULL) status(outcome, OS64_JS_BAD_ARGUMENT, "context is not a runtime member");
    return realm;
}

JS_PUBLIC os64_js_status_t os64_js_realm_create(os64_js_runtime_t *runtime, const char *caller_abi,
                                                os64_js_realm_t **out, os64_js_outcome_t *outcome)
{
    if (outcome == NULL) return OS64_JS_BAD_ARGUMENT;
    if (outcome_in_use(runtime, outcome)) return OS64_JS_BUSY;
    if (out != NULL) *out = NULL;
    if (os64_js_context(runtime, caller_abi, outcome) == NULL) return outcome->status;
    if (out == NULL) return status(outcome, OS64_JS_BAD_ARGUMENT, "realm output is required");
    if (runtime->realm_count == OS64_JS_MAX_REALMS)
        return status(outcome, OS64_JS_REALM_LIMIT, "runtime realm slots are full");
    os64_js_realm_t *realm = &runtime->realms[runtime->realm_count];
    os64_memset(realm, 0, sizeof(*realm));
    realm->runtime = runtime;
    realm->promise_then = JS_UNDEFINED;
    SetupCall call;
    os64_js_status_t result = setup_enter(realm, outcome, true, &call);
    if (result != OS64_JS_OK) return result;
    result = setup_prepare(&call, false, 0, "<realm setup>");
    if (result != OS64_JS_OK) return result;
    bool ready = !observe(runtime) && realm_initialize(realm);
    if (!ready && !observe(runtime))
        latch(runtime, OS64_JS_HOST_FAILURE, OS64_JS_LIMIT_NONE, 0);
    result = setup_leave(&call);
    if (result != OS64_JS_OK) {
        if (realm->context != NULL) {
            JS_FreeValue(realm->context, realm->promise_then);
            JS_FreeContext(realm->context);
            realm->context = NULL;
            realm->promise_then = JS_UNDEFINED;
        }
    } else {
        runtime->realm_count++;
        *out = realm;
    }
    return result;
}

JS_PUBLIC os64_js_status_t os64_js_realm_preflight(os64_js_runtime_t *runtime,
                                                 const char *caller_abi,
                                                 size_t required_headroom,
                                                 os64_js_outcome_t *outcome)
{
    if (outcome == NULL) return OS64_JS_BAD_ARGUMENT;
    if (outcome_in_use(runtime, outcome)) return OS64_JS_BUSY;
    if (os64_js_context(runtime, caller_abi, outcome) == NULL) return outcome->status;
    if (required_headroom == 0)
        return status(outcome, OS64_JS_BAD_ARGUMENT, "positive admission headroom is required");
    /* Raw engine allocation can fail inside a callback before observe() has
     * latched it. Do not disguise that failure as a local admission refusal. */
    if (runtime->allocator.failures != 0)
        return status(outcome, OS64_JS_FAILED_RUNTIME, "engine allocator has failed");
    if (runtime->realm_count == OS64_JS_MAX_REALMS)
        return status(outcome, OS64_JS_REALM_LIMIT, "runtime realm slots are full");
    if (required_headroom > os64_js_heap_headroom(runtime))
        return status(outcome, OS64_JS_ADMISSION_REFUSED, "insufficient engine admission headroom");
    return OS64_JS_OK;
}

JS_PUBLIC os64_js_status_t os64_js_realm_create_with_headroom(os64_js_runtime_t *runtime,
                                                            const char *caller_abi,
                                                            size_t required_headroom,
                                                            os64_js_realm_t **out,
                                                            os64_js_outcome_t *outcome)
{
    if (outcome == NULL) return OS64_JS_BAD_ARGUMENT;
    if (outcome_in_use(runtime, outcome)) return OS64_JS_BUSY;
    if (out != NULL) *out = NULL;
    os64_js_status_t result = os64_js_realm_preflight(runtime, caller_abi, required_headroom, outcome);
    if (result != OS64_JS_OK) return result;
    return os64_js_realm_create(runtime, caller_abi, out, outcome);
}

JS_PUBLIC os64_js_status_t os64_js_check_budget(os64_js_runtime_t *runtime,
                                               const char *caller_abi,
                                               os64_js_outcome_t *outcome)
{
    if (outcome == NULL) return OS64_JS_BAD_ARGUMENT;
    if (outcome_in_use(runtime, outcome)) return OS64_JS_BUSY;
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
 *     kind                'c', a classic script, or 'm', a module; three
 *                         zero bytes after
 *     payload length      8 bytes, little-endian
 *     payload digest      FNV-1a 64 of the payload, 8 bytes, little-endian
 *     a module's name     its length (8 bytes, little-endian), then its
 *                         bytes; a classic script has none
 *
 * JS_ReadObject trusts what it reads, so nothing reaches it that this head
 * does not describe exactly: the identity says the engine that wrote it is
 * this one (a patch can change what the compiler emits without changing
 * upstream's BC_VERSION), and the digest that the bytes are the bytes it
 * wrote. A module's name is in the head because reading a module enters it
 * in the context's modules under the name it was written with, which is
 * its identity for resolution, and no read undoes that: a module blob is
 * read only by a compile of that same name (BYTECODE_CACHE.md § 9). */
#define BLOB_MAGIC "os64-jsbc 1\n"
#define BLOB_MAGIC_LENGTH 12
#define BLOB_ENGINE_LENGTH 64
#define BLOB_KIND_CLASSIC 'c'
#define BLOB_KIND_MODULE 'm'
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

/* The payload of a blob this engine wrote of `want` kind — a module's, of
 * the module `name` — or NULL. */
static const uint8_t *blob_payload(const uint8_t *blob, size_t length, char want, const char *name,
                                   size_t *payload_length)
{
    if (blob == NULL || length < BLOB_HEAD ||
        os64_memcmp(blob, BLOB_MAGIC, BLOB_MAGIC_LENGTH) != 0 ||
        os64_memcmp(blob + BLOB_MAGIC_LENGTH, OS64_JS_ENGINE_ID, BLOB_ENGINE_LENGTH) != 0)
        return NULL;
    const uint8_t *kind = blob + BLOB_MAGIC_LENGTH + BLOB_ENGINE_LENGTH;
    if (kind[0] != want || kind[1] != 0 || kind[2] != 0 || kind[3] != 0)
        return NULL;
    size_t at = BLOB_HEAD;
    if (want == BLOB_KIND_MODULE) {
        size_t nlen = os64_strlen(name);
        if (length - at < 8 || get_u64(blob + at) != nlen || length - at - 8 < nlen ||
            os64_memcmp(blob + at + 8, name, nlen) != 0)
            return NULL;
        at += 8 + nlen;
    }
    uint64_t said = get_u64(kind + 4);
    if (said != length - at) return NULL;
    const uint8_t *payload = blob + at;
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
static void blob_keep(os64_js_realm_t *realm, JSValueConst value, os64_js_bytecode_t *bytecode, char kind_of,
                      const char *name)
{
    os64_js_runtime_t *runtime = realm != NULL ? realm->runtime : NULL;
    unsigned failures = runtime->allocator.failures;
    size_t size = 0;
    uint8_t *payload = JS_WriteObject(realm->context, &size, value, JS_WRITE_OBJ_BYTECODE);
    if (payload == NULL || runtime->allocator.failures != failures) {
        if (payload != NULL) js_free(realm->context, payload);
        discard_exception(realm->context);
        runtime->allocator.failures = failures;
        return;
    }
    size_t nlen = kind_of == BLOB_KIND_MODULE ? os64_strlen(name) : 0;
    size_t head = BLOB_HEAD + (kind_of == BLOB_KIND_MODULE ? 8 + nlen : 0);
    uint8_t *blob = size <= SIZE_MAX - head ? os64_malloc(head + size) : NULL;
    if (blob != NULL) {
        os64_memcpy(blob, BLOB_MAGIC, BLOB_MAGIC_LENGTH);
        os64_memcpy(blob + BLOB_MAGIC_LENGTH, OS64_JS_ENGINE_ID, BLOB_ENGINE_LENGTH);
        uint8_t *kind = blob + BLOB_MAGIC_LENGTH + BLOB_ENGINE_LENGTH;
        kind[0] = (uint8_t)kind_of;
        kind[1] = kind[2] = kind[3] = 0;
        put_u64(kind + 4, size);
        put_u64(kind + 12, blob_digest(payload, size));
        if (kind_of == BLOB_KIND_MODULE) {
            put_u64(blob + BLOB_HEAD, nlen);
            os64_memcpy(blob + BLOB_HEAD + 8, name, nlen);
        }
        os64_memcpy(blob + head, payload, size);
        bytecode->kept = blob;
        bytecode->kept_length = head + size;
    }
    js_free(realm->context, payload);
}

/* The script to run: the blob's, read under `source_name`, when it is this
 * engine's and whole; else the source compiled, its blob kept when asked.
 * A read the engine refuses is a miss like any other, unless the runtime
 * failed under it (its memory, its deadline), which is sticky as a
 * compile's failure would be. Unlike the writer (blob_keep), the reader
 * answers every allocation it is refused with an exception: the suite's
 * read sweep refuses each in turn, and none comes back a function. */
static JSValue prepare(os64_js_realm_t *realm, const char *source, size_t length,
                       const char *source_name, os64_js_bytecode_t *bytecode)
{
    os64_js_runtime_t *runtime = realm != NULL ? realm->runtime : NULL;
    size_t payload_length = 0;
    const uint8_t *payload =
        blob_payload(bytecode->blob, bytecode->blob_length, BLOB_KIND_CLASSIC, NULL, &payload_length);
    if (payload != NULL) {
        JSValue function = JS_ReadObjectNamed(realm->context, payload, payload_length,
                                              JS_READ_OBJ_BYTECODE, source_name);
        if (JS_VALUE_GET_TAG(function) == JS_TAG_FUNCTION_BYTECODE) {
            bytecode->from_blob = true;
            return function;
        }
        if (JS_IsException(function)) {
            if (observe(runtime)) return function;
            discard_exception(realm->context);
        } else {
            JS_FreeValue(realm->context, function);
        }
    }
    JSValue function = JS_Eval(realm->context, source, length, source_name,
                               JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (!JS_IsException(function) && bytecode->keep) blob_keep(realm, function, bytecode, BLOB_KIND_CLASSIC, NULL);
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

static JSValue round_trip(os64_js_realm_t *realm, const char *source, size_t length,
                          const char *source_name)
{
    os64_js_runtime_t *runtime = realm != NULL ? realm->runtime : NULL;
    os64_js_bytecode_t written = {.keep = true};
    JSValue function = prepare(realm, source, length, "os64-round-trip", &written);
    if (JS_IsException(function)) {
        if (observe(runtime)) return function;
        discard_exception(realm->context);
        return JS_Eval(realm->context, source, length, source_name,
                       JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    }
    JS_FreeValue(realm->context, function);
    if (written.kept == NULL) {
        if (observe(runtime)) return JS_EXCEPTION;
        os64_js_round_trips_skipped++;
        return JS_Eval(realm->context, source, length, source_name,
                       JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    }
    os64_js_bytecode_t read = {.blob = written.kept, .blob_length = written.kept_length};
    function = prepare(realm, source, length, source_name, &read);
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
static os64_js_status_t evaluate(os64_js_realm_t *realm, const char *source,
                                size_t length, const char *source_name,
                                os64_js_bytecode_t *bytecode, os64_js_outcome_t *outcome)
{
    os64_js_runtime_t *runtime = realm != NULL ? realm->runtime : NULL;
    int64_t now = os64_micros();
    if (now < 0) latch(runtime, OS64_JS_HOST_FAILURE, OS64_JS_LIMIT_NONE, now);
    else if (runtime->limits.execution_ms * 1000 > (uint64_t)(INT64_MAX - now))
        return OS64_JS_BAD_ARGUMENT;
    else {
        runtime->jobs = 0;
        runtime->source_truncated = copy_text(runtime->source_name, sizeof(runtime->source_name), source_name);
        runtime->deadline = now + (int64_t)(runtime->limits.execution_ms * 1000);
        runtime->turn = true;
        runtime->turn_owner = realm;
        realm->evaluated = true;
        JSValue value;
#ifdef OS64_JS_BYTECODE_ROUND_TRIP
        if (bytecode == NULL) {
            value = round_trip(realm, source, length, source_name);
            if (!JS_IsException(value)) value = JS_EvalFunction(realm->context, value);
        } else {
#else
        if (bytecode == NULL) {
            value = JS_Eval(realm->context, source, length, source_name, JS_EVAL_TYPE_GLOBAL);
        } else {
#endif
            value = prepare(realm, source, length, source_name, bytecode);
            if (!JS_IsException(value)) value = JS_EvalFunction(realm->context, value);
        }
        if (JS_IsException(value)) {
            JSValue error = JS_GetException(realm->context);
            caught(runtime, realm->context, error);
            outcome->status = OS64_JS_EXCEPTION;
            diagnostic(runtime, realm->context, error, outcome);
            JS_FreeValue(realm->context, error);
        }
        JS_FreeValue(realm->context, value);
    }
    return OS64_JS_OK;
}

static os64_js_status_t eval_text(os64_js_realm_t *realm, const void *source, size_t length,
                                  const char *source_name, os64_js_bytecode_t *bytecode,
                                  os64_js_outcome_t *outcome)
{
    os64_js_runtime_t *runtime = realm != NULL ? realm->runtime : NULL;
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
    result = evaluate(realm, copy, length, source_name, bytecode, outcome);
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
    os64_js_realm_t *realm = primary(runtime);
    return eval_text(realm, source, length, source_name, NULL, outcome);
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
        runtime->ran_jobs = true;
        JSContext *context = NULL;
        if (JS_ExecutePendingJob(runtime->engine, &context) < 0) {
            if (context == NULL) jsport_fatal("job context lifetime", __FILE__, __LINE__);
            JSValue error = JS_GetException(context);
            caught(runtime, context, error);
            outcome->status = OS64_JS_EXCEPTION;
            diagnostic(runtime, context, error, outcome);
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

JS_PUBLIC os64_js_status_t os64_js_realm_run(os64_js_realm_t *realm,
                                     const void *source, size_t length,
                                     const char *source_name,
                                     os64_js_outcome_t *outcome)
{
    os64_js_runtime_t *runtime = realm != NULL ? realm->runtime : NULL;
    return complete_run(runtime, eval_text(realm, source, length, source_name, NULL, outcome), outcome);
}

JS_PUBLIC os64_js_status_t os64_js_realm_run_bytecode(os64_js_realm_t *realm,
                                              const void *source, size_t length,
                                              const char *source_name,
                                              os64_js_bytecode_t *bytecode,
                                              os64_js_outcome_t *outcome)
{
    os64_js_runtime_t *runtime = realm != NULL ? realm->runtime : NULL;
    if (bytecode == NULL) {
        os64_js_status_t result = enter(runtime, outcome);
        if (result != OS64_JS_OK) return result;
        return refuse(runtime, outcome, OS64_JS_BAD_ARGUMENT, "bytecode is required");
    }
    bytecode->kept = NULL;
    bytecode->kept_length = 0;
    bytecode->from_blob = false;
    return complete_run(runtime, eval_text(realm, source, length, source_name, bytecode, outcome),
                        outcome);
}

JS_PUBLIC os64_js_status_t os64_js_run_file(os64_js_runtime_t *runtime,
                                          const char *path,
                                          os64_js_outcome_t *outcome)
{
    os64_js_realm_t *realm = primary(runtime);
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
        result = evaluate(realm, source, length, path, NULL, outcome);
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

JS_PUBLIC os64_js_status_t os64_js_realm_task_begin(os64_js_realm_t *realm, const char *caller_abi,
                                            const char *source_name, os64_js_outcome_t *outcome)
{
    os64_js_runtime_t *runtime = realm != NULL ? realm->runtime : NULL;
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
    runtime->turn_owner = realm;
    runtime->task = true;
    realm->evaluated = true;
    return leave(runtime, outcome);
}

JS_PUBLIC os64_js_status_t os64_js_realm_call(os64_js_realm_t *realm, const char *caller_abi,
                                      JSValue function, JSValue this_value, int argc,
                                      JSValue *argv, bool *returned_false,
                                      os64_js_outcome_t *outcome)
{
    os64_js_runtime_t *runtime = realm != NULL ? realm->runtime : NULL;
    if (returned_false != NULL) *returned_false = false;
    os64_js_status_t result = enter_task(runtime, caller_abi, true, outcome);
    if (result != OS64_JS_OK) return result;
    if (argc < 0 || (argc != 0 && argv == NULL))
        return refuse(runtime, outcome, OS64_JS_BAD_ARGUMENT, "argument array is invalid");
    if (observe(runtime)) return leave(runtime, outcome);
    realm->evaluated = true;
    JSValue value = JS_Call(realm->context, function, this_value, argc, argv);
    if (JS_IsException(value)) {
        JSValue error = JS_GetException(realm->context);
        caught(runtime, realm->context, error);
        outcome->status = OS64_JS_EXCEPTION;
        diagnostic(runtime, realm->context, error, outcome);
        JS_FreeValue(realm->context, error);
    } else if (returned_false != NULL && JS_VALUE_GET_TAG(value) == JS_TAG_BOOL &&
               !JS_VALUE_GET_BOOL(value)) {
        *returned_false = true;
    }
    JS_FreeValue(realm->context, value);
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
        runtime->ran_jobs = true;
        JSContext *context = NULL;
        if (JS_ExecutePendingJob(runtime->engine, &context) < 0) {
            if (context == NULL) jsport_fatal("job context lifetime", __FILE__, __LINE__);
            JSValue error = JS_GetException(context);
            caught(runtime, context, error);
            if (outcome->status == OS64_JS_OK) {
                outcome->status = OS64_JS_EXCEPTION;
                diagnostic(runtime, context, error, outcome);
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
    if (outcome_in_use(runtime, outcome)) return OS64_JS_BUSY;
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

JS_PUBLIC size_t os64_js_heap_peak(const os64_js_runtime_t *runtime)
{
    return runtime != NULL ? runtime->allocator.peak_bytes : 0;
}

JS_PUBLIC size_t os64_js_heap_bytes(const os64_js_runtime_t *runtime)
{
    return runtime != NULL ? runtime->allocator.charged_bytes : 0;
}

JS_PUBLIC size_t os64_js_heap_headroom(const os64_js_runtime_t *runtime)
{
    if (runtime == NULL || runtime->allocator.charged_bytes >= runtime->allocator.allocation_limit)
        return 0;
    return runtime->allocator.allocation_limit - runtime->allocator.charged_bytes;
}

JS_PUBLIC bool os64_js_heap_census(os64_js_runtime_t *runtime, os64_js_heap_census_t *out)
{
    if (out == NULL) return false;
    os64_memset(out, 0, sizeof(*out));
    if (runtime == NULL || runtime->engine == NULL || runtime->failure != OS64_JS_OK || runtime->active)
        return false;
    JSMemoryUsage use;
    JS_ComputeMemoryUsage(runtime->engine, &use);
    out->objects = (uint64_t)use.obj_count;
    out->object_bytes = (uint64_t)use.obj_size;
    out->properties = (uint64_t)use.prop_count;
    out->property_bytes = (uint64_t)use.prop_size;
    out->strings = (uint64_t)use.str_count;
    out->string_bytes = (uint64_t)use.str_size;
    out->atoms = (uint64_t)use.atom_count;
    out->atom_bytes = (uint64_t)use.atom_size;
    out->shapes = (uint64_t)use.shape_count;
    out->shape_bytes = (uint64_t)use.shape_size;
    out->functions = (uint64_t)use.js_func_count;
    out->function_bytes = (uint64_t)use.js_func_size;
    out->function_code_bytes = (uint64_t)use.js_func_code_size;
    out->arrays = (uint64_t)use.array_count;
    out->fast_array_elements = (uint64_t)use.fast_array_elements;
    out->binary_objects = (uint64_t)use.binary_object_count;
    out->binary_bytes = (uint64_t)use.binary_object_size;
    out->blocks = (uint64_t)use.malloc_count;
    out->charged_bytes = runtime->allocator.charged_bytes;
    return true;
}

/* Not through enter(): a host clears itself from a runtime that has failed
 * as readily as from one that has not, before its opaque goes away. */
JS_PUBLIC os64_js_status_t os64_js_realm_set_script_host(os64_js_realm_t *realm, const char *caller_abi,
                                                 const os64_js_script_host_t *host,
                                                 os64_js_outcome_t *outcome)
{
    os64_js_runtime_t *runtime = realm != NULL ? realm->runtime : NULL;
    if (outcome == NULL) return OS64_JS_BAD_ARGUMENT;
    if (outcome_in_use(runtime, outcome)) return OS64_JS_BUSY;
    os64_memset(outcome, 0, sizeof(*outcome));
    if (runtime == NULL || caller_abi == NULL)
        return status(outcome, OS64_JS_BAD_ARGUMENT, "runtime and ABI are required");
    if (os64_strcmp(caller_abi, OS64_JS_ABI_ID) != 0)
        return status(outcome, OS64_JS_ABI_MISMATCH, "binding header ABI mismatch");
    if (runtime->destroying) return status(outcome, OS64_JS_FAILED_RUNTIME, "runtime is being destroyed");
    if (runtime->active && (host == NULL || realm->evaluated || realm->host_configured)) return status(outcome, OS64_JS_BUSY, "runtime is executing");
    realm->hosted = host != NULL;
    realm->host_configured |= host != NULL;
    if (host != NULL) realm->host = *host;
    else os64_memset(&realm->host, 0, sizeof(realm->host));
    return OS64_JS_OK;
}

JS_PUBLIC bool os64_js_realm_take_rejection(os64_js_realm_t *realm, const char *caller_abi,
                                     JSValue *promise, JSValue *reason, uint32_t *tag)
{
    os64_js_runtime_t *runtime = realm != NULL ? realm->runtime : NULL;
    if (promise != NULL) *promise = JS_UNDEFINED;
    if (reason != NULL) *reason = JS_UNDEFINED;
    if (tag != NULL) *tag = 0;
    if (runtime == NULL || caller_abi == NULL || promise == NULL || reason == NULL ||
        os64_strcmp(caller_abi, OS64_JS_ABI_ID) != 0 || !realm->hosted || runtime->active ||
        runtime->failure != OS64_JS_OK || runtime->rejections == NULL || JS_IsJobPending(runtime->engine))
        return false;
    Rejection *item = runtime->rejections;
    while (item != NULL && &runtime->realms[item->realm_index] != realm) item = item->next;
    if (item == NULL) return false;
    rejection_unlink(runtime, rejection_place(runtime, JS_VALUE_GET_PTR(item->promise)));
    *promise = item->promise;
    *reason = item->reason;
    if (tag != NULL) *tag = item->tag;
    js_free_rt(runtime->engine, item);
    return true;
}

static bool write_output(os64_js_realm_t *realm, const char *bytes, size_t size)
{
    os64_js_runtime_t *runtime = realm != NULL ? realm->runtime : NULL;
    while (size != 0 && !observe(runtime)) {
        int64_t n = os64_write(realm->output_handle, bytes, size);
        if (n <= 0 || (uint64_t)n > size) {
            latch(runtime, OS64_JS_HOST_FAILURE, OS64_JS_LIMIT_NONE, n < 0 ? n : 0);
            return false;
        }
        bytes += (size_t)n;
        size -= (size_t)n;
    }
    return !observe(runtime);
}

/* Deeper console groups still count; their lines stop moving right. */
#define CONSOLE_INDENT_MAX 32

static const char kSpaces[2 * CONSOLE_INDENT_MAX] =
    "                                                                ";

/* Two spaces for each of `depth` open groups, capped. */
static size_t indent_size(uint32_t depth)
{
    return 2 * (size_t)(depth < CONSOLE_INDENT_MAX ? depth : CONSOLE_INDENT_MAX);
}

static bool write_indent(os64_js_realm_t *realm, uint32_t depth)
{
    return write_output(realm, kSpaces, indent_size(depth));
}

/* Where an Error is written (write_error): its pieces gather here and reach
 * the output a buffer at a time, each line after the first indented as the
 * first was. `indent` is a line's indent owed once something follows its
 * line break, so no line is only an indent. */
typedef struct {
    os64_js_realm_t *realm;
    uint32_t depth;
    bool failed, indent;
    size_t used;
    char buffer[512];
} ErrorSink;

static void sink_flush(ErrorSink *sink)
{
    if (sink->used != 0 && !sink->failed && !write_output(sink->realm, sink->buffer, sink->used))
        sink->failed = true;
    sink->used = 0;
}

static void sink_put(ErrorSink *sink, const char *bytes, size_t size)
{
    while (size != 0 && !sink->failed) {
        if (sink->used == sizeof(sink->buffer)) sink_flush(sink);
        size_t room = sizeof(sink->buffer) - sink->used, n = size < room ? size : room;
        os64_memcpy(sink->buffer + sink->used, bytes, n);
        sink->used += n;
        bytes += n;
        size -= n;
    }
}

/* Each line after the first indented as the first was. */
static void error_sink(void *opaque, const char *bytes, size_t size)
{
    ErrorSink *sink = opaque;
    while (size != 0 && !sink->failed) {
        if (sink->indent && bytes[0] != '\n') {
            sink_put(sink, kSpaces, indent_size(sink->depth));
            sink->indent = false;
        }
        size_t line = 0;
        while (line < size && bytes[line] != '\n') line++;
        bool newline = line < size;
        if (newline) line++;
        sink_put(sink, bytes, line);
        if (newline) sink->indent = true;
        bytes += line;
        size -= line;
    }
}

/* `key` as a string `object` holds as data, else JS_UNDEFINED: a getter is
 * not called, and `object` is the Error or an ordinary prototype, whose own
 * properties are read without running page code. -1 with the exception
 * pending when the engine cannot allocate. */
static int error_field(JSContext *context, JSValueConst object, const char *key, JSValue *out)
{
    *out = JS_UNDEFINED;
    JSAtom atom = JS_NewAtom(context, key);
    if (atom == JS_ATOM_NULL) return -1;
    JSPropertyDescriptor desc;
    int has = JS_GetOwnProperty(context, &desc, object, atom);
    JS_FreeAtom(context, atom);
    if (has <= 0) return has;
    if (!(desc.flags & JS_PROP_GETSET) && JS_IsString(desc.value)) *out = desc.value;
    else JS_FreeValue(context, desc.value);
    JS_FreeValue(context, desc.getter);
    JS_FreeValue(context, desc.setter);
    return 0;
}

/* `field`, a string or undefined, through the sink; it is let go of. */
static int sink_string(ErrorSink *sink, JSContext *context, JSValue field)
{
    if (JS_IsUndefined(field)) return 0;
    size_t length;
    const char *text = JS_ToCStringLen(context, &length, field);
    JS_FreeValue(context, field);
    if (text == NULL) return -1;
    error_sink(sink, text, length);
    JS_FreeCString(context, text);
    return 0;
}

/* An Error as other browsers' consoles print one, with no page code run:
 * its head, the name ("Error" without one) and then ": " and the message
 * when there is one, each a string the Error holds or, failing that, its
 * prototype does when that is an ordinary object (a Proxy's traps would be
 * page code: DEBTS.md); then its own stack on the lines after, less one
 * line break at its end. -1 with the exception pending when the engine
 * cannot allocate. */
static int write_error(ErrorSink *sink, JSContext *context, JSValueConst error)
{
    JSValue fields[3] = {JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED};
    static const char *const kKeys[3] = {"name", "message", "stack"};
    JSValue prototype = JS_GetPrototype(context, error), plain = JS_NewObject(context);
    int status = JS_IsException(prototype) || JS_IsException(plain) ? -1 : 0;
    bool ordinary = status == 0 && JS_IsObject(prototype) &&
                    (JS_GetClassID(prototype) == JS_GetClassID(plain) ||
                     JS_GetClassID(prototype) == JS_GetClassID(error));
    for (int i = 0; i < 3 && status == 0; i++) {
        status = error_field(context, error, kKeys[i], &fields[i]);
        if (status == 0 && i < 2 && JS_IsUndefined(fields[i]) && ordinary)
            status = error_field(context, prototype, kKeys[i], &fields[i]);
    }
    JS_FreeValue(context, prototype);
    JS_FreeValue(context, plain);
    if (status == 0 && JS_IsUndefined(fields[0])) error_sink(sink, "Error", 5);
    if (status == 0) status = sink_string(sink, context, fields[0]);
    else JS_FreeValue(context, fields[0]);
    fields[0] = JS_UNDEFINED;
    if (status == 0 && !JS_IsUndefined(fields[1])) {
        size_t length;
        const char *text = JS_ToCStringLen(context, &length, fields[1]);
        if (text == NULL) status = -1;
        else {
            if (length != 0) {
                error_sink(sink, ": ", 2);
                error_sink(sink, text, length);
            }
            JS_FreeCString(context, text);
        }
    }
    if (status == 0 && !JS_IsUndefined(fields[2])) {
        size_t length;
        const char *text = JS_ToCStringLen(context, &length, fields[2]);
        if (text == NULL) status = -1;
        else {
            error_sink(sink, "\n", 1);
            error_sink(sink, text, length != 0 && text[length - 1] == '\n' ? length - 1 : length);
            JS_FreeCString(context, text);
        }
    }
    for (int i = 0; i < 3; i++) JS_FreeValue(context, fields[i]);
    return status;
}

/* Each value by QuickJS string conversion, spaces between them, but an
 * Error as write_error writes one. `depth` is the open console groups its
 * later lines are indented for. */
static bool write_values(os64_js_realm_t *realm, JSContext *context, int argc,
                         JSValueConst *args, uint32_t depth, bool *thrown)
{
    os64_js_runtime_t *runtime = realm != NULL ? realm->runtime : NULL;
    for (int i = 0; i < argc && !observe(runtime); i++) {
        if (JS_IsError(context, args[i])) {
            ErrorSink sink = {.realm = realm, .depth = depth};
            if (i != 0 && !write_output(realm, " ", 1)) return false;
            int status = write_error(&sink, context, args[i]);
            sink_flush(&sink);
            if (status < 0) {
                *thrown = true;
                return false;
            }
            if (sink.failed) return false;
            continue;
        }
        size_t length;
        const char *text = JS_ToCStringLen(context, &length, args[i]);
        if (text == NULL) {
            *thrown = true;
            return false;
        }
        bool written = (i == 0 || write_output(realm, " ", 1)) &&
                       write_output(realm, text, length);
        JS_FreeCString(context, text);
        if (!written) return false;
    }
    return !observe(runtime);
}

static JSValue output_ready(os64_js_realm_t *realm, JSContext *context)
{
    os64_js_runtime_t *runtime = realm != NULL ? realm->runtime : NULL;
    if (!runtime->active || !runtime->turn)
        jsport_fatal("output outside a runtime turn", __FILE__, __LINE__);
    /* Property traps can see a staged function before setup commits. It must
     * not use a borrowed handle until the entire installation succeeds. */
    if (!realm->output_installed)
        return JS_ThrowTypeError(context, "output installation is incomplete");
    return JS_UNDEFINED;
}

static JSValue output_call(JSContext *context, JSValueConst self, int argc,
                           JSValueConst *args)
{
    (void)self;
    os64_js_realm_t *realm = JS_GetContextOpaque(context);
    JSValue ready = output_ready(realm, context);
    if (JS_IsException(ready)) return ready;
    bool thrown = false;
    if (write_values(realm, context, argc, args, 0, &thrown) && write_output(realm, "\n", 1))
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

/* One console line: the indent, the `head` pieces run together, then the
 * values (a space between the head and them). */
static JSValue console_line(os64_js_realm_t *realm, JSContext *context,
                            const char *const *head, size_t pieces, int argc, JSValueConst *args)
{
    bool thrown = false;
    bool written = write_indent(realm, realm->console_depth);
    for (size_t i = 0; written && i < pieces; i++)
        written = write_output(realm, head[i], os64_strlen(head[i]));
    written = written && (pieces == 0 || argc == 0 || write_output(realm, " ", 1)) &&
              write_values(realm, context, argc, args, realm->console_depth, &thrown) &&
              write_output(realm, "\n", 1);
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
static JSValue console_labelled(os64_js_realm_t *realm, JSContext *context, JSAtom label,
                                const char *before, const char *after, int argc, JSValueConst *args)
{
    const char *name = JS_AtomToCString(context, label);
    if (name == NULL) return JS_EXCEPTION;
    const char *said[] = {name, ": ", after};
    const char *warned[] = {before, " '", name, "' ", after};
    JSValue result = before == NULL ? console_line(realm, context, said, 3, argc, args)
                                    : console_line(realm, context, warned, 5, argc, args);
    JS_FreeCString(context, name);
    return result;
}

static JSValue console_call(JSContext *context, JSValueConst self, int argc,
                            JSValueConst *args, int kind, JSValue *data)
{
    (void)self;
    os64_js_realm_t *realm = data_realm(context, data[2]);
    context = realm->context;
    JSValue ready = output_ready(realm, context);
    if (JS_IsException(ready)) return ready;
    switch (kind) {
    case CONSOLE_LOG:
        return console_line(realm, context, NULL, 0, argc, args);
    case CONSOLE_ASSERT: {
        if (argc > 0 && JS_ToBool(context, args[0])) return JS_UNDEFINED;
        /* A message that is a string ends the sentence; any other is a value. */
        if (argc > 1 && JS_IsString(args[1])) {
            const char *first = JS_ToCString(context, args[1]);
            if (first == NULL) return JS_EXCEPTION;
            const char *head[] = {"Assertion failed: ", first};
            JSValue result = console_line(realm, context, head, 2, argc - 2, args + 2);
            JS_FreeCString(context, first);
            return result;
        }
        const char *head[] = {"Assertion failed"};
        return console_line(realm, context, head, 1, argc > 1 ? argc - 1 : 0, args + 1);
    }
    case CONSOLE_CLEAR:
        /* A stream cannot be cleared; the groups it was in can. */
        realm->console_depth = 0;
        return JS_UNDEFINED;
    case CONSOLE_GROUP: {
        JSValue result = argc > 0 ? console_line(realm, context, NULL, 0, argc, args) : JS_UNDEFINED;
        if (!JS_IsException(result) && realm->console_depth != UINT32_MAX) realm->console_depth++;
        return result;
    }
    case CONSOLE_GROUP_END:
        if (realm->console_depth != 0) realm->console_depth--;
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
            result = console_labelled(realm, context, label, NULL, text, 0, NULL);
    } else if (kind == CONSOLE_COUNT_RESET) {
        if (!has)
            result = console_labelled(realm, context, label, "Count for", "does not exist", 0, NULL);
        else if (JS_SetProperty(context, table, label, JS_NewInt32(context, 0)) < 0)
            result = JS_EXCEPTION;
    } else if (kind == CONSOLE_TIME) {
        if (has)
            result = console_labelled(realm, context, label, "Timer", "already exists", 0, NULL);
        else if (JS_SetProperty(context, table, label, JS_NewInt64(context, os64_micros())) < 0)
            result = JS_EXCEPTION;
    } else if (!has) {
        result = console_labelled(realm, context, label, "Timer", "does not exist", 0, NULL);
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
            result = console_labelled(realm, context, label, NULL, text,
                                      kind == CONSOLE_TIME_LOG && argc > 1 ? argc - 1 : 0, args + 1);
    }
    JS_FreeAtom(context, label);
    return result;
}

/* A method named as the standard names it (data functions are born nameless). */
static JSValue console_method(JSContext *context, size_t i, JSValue *tables)
{
    JSValue method = JS_NewCFunctionData(context, console_call, 0, console_methods[i].kind, 3, tables);
    if (JS_IsException(method)) return method;
    if (JS_DefinePropertyValueStr(context, method, "name", JS_NewString(context, console_methods[i].name),
                                  JS_PROP_CONFIGURABLE) < 0) {
        JS_FreeValue(context, method);
        return JS_EXCEPTION;
    }
    return method;
}

static void setup_exception(os64_js_realm_t *realm, os64_js_outcome_t *outcome)
{
    os64_js_runtime_t *runtime = realm != NULL ? realm->runtime : NULL;
    JSValue error = JS_GetException(realm->context);
    outcome->status = OS64_JS_EXCEPTION;
    diagnostic(runtime, realm->context, error, outcome);
    JS_FreeValue(realm->context, error);
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

static void restore_output_property(os64_js_realm_t *realm, OutputProperty *property)
{
    os64_js_runtime_t *runtime = realm != NULL ? realm->runtime : NULL;
    JSContext *context = realm->context;
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

static os64_js_status_t install_output(os64_js_realm_t *realm,
                                                int32_t handle, uint32_t names,
                                                os64_js_outcome_t *outcome, bool nested)
{
    os64_js_runtime_t *runtime = realm != NULL ? realm->runtime : NULL;
    SetupCall call;
    os64_js_status_t result = setup_enter(realm, outcome, nested, &call);
    if (result != OS64_JS_OK) return result;
    const uint32_t valid = OS64_JS_OUTPUT_PRINT | OS64_JS_OUTPUT_CONSOLE;
    if (handle < 0 || names == 0 || (names & ~valid) != 0)
        return setup_refuse(&call, OS64_JS_BAD_ARGUMENT, "output handle and selected names are required");
    result = setup_prepare(&call, realm->output_installed, 1, "<output setup>");
    if (result != OS64_JS_OK) return result;
    if (observe(runtime)) return setup_leave(&call);
    JSContext *context = realm->context;
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
        /* Each method retains its label tables and installing realm slot. */
        JSValue tables[3] = {JS_UNDEFINED, JS_UNDEFINED,
                            JS_NewInt32(context, (int32_t)(realm - runtime->realms))};
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
            if (properties[i-1].attempted) restore_output_property(realm, &properties[i-1]);
        if (installed < 0) {
            outcome->status = OS64_JS_EXCEPTION;
            diagnostic(runtime, realm->context, error, outcome);
        }
        JS_FreeValue(context, error);
    } else {
        realm->output_handle = handle;
        realm->output_installed = true;
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
    return setup_leave(&call);
}

JS_PUBLIC os64_js_status_t os64_js_install_args(os64_js_runtime_t *runtime,
                                              size_t count, const char *const *args,
                                              os64_js_outcome_t *outcome)
{
    os64_js_realm_t *realm = primary(runtime);
    SetupCall call;
    os64_js_status_t result = setup_enter(realm, outcome, false, &call);
    if (result != OS64_JS_OK) return result;
    if (count > UINT32_MAX || (count != 0 && args == NULL))
        return setup_refuse(&call, OS64_JS_BAD_ARGUMENT, "argument array is invalid");
    for (size_t i = 0; i < count; i++)
        if (args[i] == NULL)
            return setup_refuse(&call, OS64_JS_BAD_ARGUMENT, "argument entries must be strings");
    result = setup_prepare(&call, realm->args_installed, 4, "<argument setup>");
    if (result != OS64_JS_OK) return result;
    if (observe(runtime)) return setup_leave(&call);
    JSContext *context = realm->context;
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
    if (installed < 0) setup_exception(realm, outcome);
    else if (!observe(runtime)) realm->args_installed = true;
    return setup_leave(&call);
}

/* ECMA-402 (INTL.md): the Intl object, made by intl.c and published as a
 * global property is (writable, configurable, not enumerable). Nothing is
 * defined unless the whole object was made; the built-ins ECMA-402
 * redefines are the script's last step (CONTRACT.md), and any refusal
 * after them (an allocation, the deadline, a cancel) is the runtime's
 * sticky failure. */
static os64_js_status_t install_intl(os64_js_realm_t *realm, const char *locale,
                                              os64_js_outcome_t *outcome, bool nested)
{
    os64_js_runtime_t *runtime = realm != NULL ? realm->runtime : NULL;
    SetupCall call;
    os64_js_status_t result = setup_enter(realm, outcome, nested, &call);
    if (result != OS64_JS_OK) return result;
    if (locale == NULL || locale[0] == '\0')
        return setup_refuse(&call, OS64_JS_BAD_ARGUMENT, "a default locale is required");
    result = setup_prepare(&call, realm->intl_installed, 2, "<intl setup>");
    if (result != OS64_JS_OK) return result;
    if (observe(runtime)) return setup_leave(&call);
    JSContext *context = realm->context;
    JSValue intl = js_intl_build(context, locale);
    int installed = JS_IsException(intl) ? -1 : 0;
    if (installed >= 0 && !observe(runtime)) {
        JSValue global = JS_GetGlobalObject(context);
        installed = JS_DefinePropertyValueStr(context, global, "Intl", intl,
                                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_THROW);
        intl = JS_UNDEFINED;
        JS_FreeValue(context, global);
    }
    JS_FreeValue(context, intl);
    if (installed < 0) setup_exception(realm, outcome);
    else if (!observe(runtime)) realm->intl_installed = true;
    return setup_leave(&call);
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
    os64_js_realm_t *realm = realm_for_context(runtime, context);
    if (realm == NULL) jsport_fatal("module context membership", __FILE__, __LINE__);
    char *name = realm->loader.resolve != NULL
        ? realm->loader.resolve(realm->loader.opaque, base, specifier) : NULL;
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
    os64_js_realm_t *realm = realm_for_context(runtime, context);
    if (realm == NULL) jsport_fatal("module context membership", __FILE__, __LINE__);
    const char *bytes = NULL;
    size_t length = 0;
    if (realm->loader.source == NULL ||
        !realm->loader.source(realm->loader.opaque, name, &bytes, &length) ||
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

JS_PUBLIC os64_js_status_t os64_js_realm_set_module_loader(os64_js_realm_t *realm, const char *caller_abi,
                                                         const os64_js_module_loader_t *loader,
                                                         os64_js_outcome_t *outcome)
{
    os64_js_runtime_t *runtime = realm != NULL ? realm->runtime : NULL;
    if (outcome == NULL) return OS64_JS_BAD_ARGUMENT;
    if (outcome_in_use(runtime, outcome)) return OS64_JS_BUSY;
    os64_memset(outcome, 0, sizeof(*outcome));
    if (runtime == NULL || caller_abi == NULL)
        return status(outcome, OS64_JS_BAD_ARGUMENT, "realm and ABI are required");
    if (os64_strcmp(caller_abi, OS64_JS_ABI_ID) != 0)
        return status(outcome, OS64_JS_ABI_MISMATCH, "binding header ABI mismatch");
    if (runtime->destroying)
        return status(outcome, OS64_JS_FAILED_RUNTIME, "runtime is being destroyed");
    if (runtime->active && (loader == NULL || realm->evaluated || realm->loader_configured))
        return status(outcome, OS64_JS_BUSY, "runtime is executing");
    if (loader != NULL) realm->loader = *loader;
    else os64_memset(&realm->loader, 0, sizeof(realm->loader));
    realm->loader_configured |= loader != NULL;
    return OS64_JS_OK;
}

/* A module compiled, or read from its blob when `bytecode` gives one framed
 * for this module's name (and is this engine's, and whole), its blob kept
 * when asked. A read the engine refuses is a miss, as a classic script's
 * is, unless the runtime failed under it. `through_bytecode` is the
 * bytecode verb's, which requires one. */
static os64_js_status_t module_compile(os64_js_realm_t *realm, const char *caller_abi, const void *source,
                                       size_t length, const char *name, const char *url, bool through_bytecode,
                                       os64_js_bytecode_t *bytecode, os64_js_module_t **module,
                                       os64_js_outcome_t *outcome);

JS_PUBLIC os64_js_status_t os64_js_realm_module_compile(os64_js_realm_t *realm, const char *caller_abi,
                                                const void *source, size_t length, const char *name,
                                                const char *url, os64_js_module_t **module,
                                                os64_js_outcome_t *outcome)
{
    return module_compile(realm, caller_abi, source, length, name, url, false, NULL, module, outcome);
}

JS_PUBLIC os64_js_status_t os64_js_realm_module_compile_bytecode(os64_js_realm_t *realm, const char *caller_abi,
                                                         const void *source, size_t length, const char *name,
                                                         const char *url, os64_js_bytecode_t *bytecode,
                                                         os64_js_module_t **module, os64_js_outcome_t *outcome)
{
    if (bytecode != NULL) {
        bytecode->kept = NULL;
        bytecode->kept_length = 0;
        bytecode->from_blob = false;
    }
    return module_compile(realm, caller_abi, source, length, name, url, true, bytecode, module, outcome);
}

static os64_js_status_t module_compile(os64_js_realm_t *realm, const char *caller_abi, const void *source,
                                       size_t length, const char *name, const char *url, bool through_bytecode,
                                       os64_js_bytecode_t *bytecode, os64_js_module_t **module,
                                       os64_js_outcome_t *outcome)
{
    os64_js_runtime_t *runtime = realm != NULL ? realm->runtime : NULL;
    if (module != NULL) *module = NULL;
    os64_js_status_t result = enter(runtime, outcome);
    if (result != OS64_JS_OK) return result;
    if (caller_abi == NULL || os64_strcmp(caller_abi, OS64_JS_ABI_ID) != 0)
        return refuse(runtime, outcome, caller_abi == NULL ? OS64_JS_BAD_ARGUMENT : OS64_JS_ABI_MISMATCH,
                      "binding header ABI mismatch");
    if ((source == NULL && length != 0) || name == NULL || module == NULL)
        return refuse(runtime, outcome, OS64_JS_BAD_ARGUMENT, "source bytes, name and handle are required");
    if (through_bytecode && bytecode == NULL)
        return refuse(runtime, outcome, OS64_JS_BAD_ARGUMENT, "bytecode is required");
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
    JSValue value = JS_UNDEFINED;
    size_t payload_length = 0;
    const uint8_t *payload = bytecode != NULL ? blob_payload(bytecode->blob, bytecode->blob_length, BLOB_KIND_MODULE,
                                                             name, &payload_length)
                                              : NULL;
    if (payload != NULL) {
        value = JS_ReadObject(realm->context, payload, payload_length, JS_READ_OBJ_BYTECODE);
        if (JS_VALUE_GET_TAG(value) == JS_TAG_MODULE) {
            bytecode->from_blob = true;
        } else if (JS_IsException(value) && !observe(runtime)) {
            discard_exception(realm->context);
            value = JS_UNDEFINED;
        } else if (!JS_IsException(value)) {
            JS_FreeValue(realm->context, value);
            value = JS_UNDEFINED;
        }
    }
    if (JS_IsUndefined(value)) {
        value = JS_Eval(realm->context, copy, length, name,
                        JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY | JS_EVAL_FLAG_NO_RESOLVE);
        if (!JS_IsException(value) && bytecode != NULL && bytecode->keep)
            blob_keep(realm, value, bytecode, BLOB_KIND_MODULE, name);
    }
    js_free_rt(runtime->engine, copy);
    if (!JS_IsException(value) &&
        module_meta(realm->context, JS_VALUE_GET_PTR(value), url != NULL ? url : name) < 0) {
        JS_FreeValue(realm->context, value);
        value = JS_EXCEPTION;
    }
    if (JS_IsException(value)) {
        js_free_rt(runtime->engine, handle);
        if (!observe(runtime)) {
            // The compile's own, not the script host's: no task is running
            // to report it, and a browser reports a module's syntax error
            // when the script that imports it runs.
            JSValue error = JS_GetException(realm->context);
            outcome->status = OS64_JS_EXCEPTION;
            diagnostic(runtime, realm->context, error, outcome);
            JS_FreeValue(realm->context, error);
        }
        return leave(runtime, outcome);
    }
    handle->realm = realm;
    handle->module = value;
    handle->promise = JS_UNDEFINED;
    handle->next = realm->modules;
    if (realm->modules != NULL) realm->modules->prev = handle;
    realm->modules = handle;
    *module = handle;
    return leave(runtime, outcome);
}

static bool module_owned(const os64_js_realm_t *realm, const os64_js_module_t *module)
{
    if (realm == NULL || module == NULL) return false;
    for (const os64_js_module_t *m = realm->modules; m != NULL; m = m->next)
        if (m == module) return true;
    return false;
}

JS_PUBLIC os64_js_status_t os64_js_realm_module_requests(os64_js_realm_t *realm, const char *caller_abi,
                                                 os64_js_module_t *module,
                                                 void (*each)(void *opaque, const char *specifier),
                                                 void *opaque, os64_js_outcome_t *outcome)
{
    os64_js_runtime_t *runtime = realm != NULL ? realm->runtime : NULL;
    os64_js_status_t result = enter(runtime, outcome);
    if (result != OS64_JS_OK) return result;
    if (caller_abi == NULL || os64_strcmp(caller_abi, OS64_JS_ABI_ID) != 0)
        return refuse(runtime, outcome, caller_abi == NULL ? OS64_JS_BAD_ARGUMENT : OS64_JS_ABI_MISMATCH,
                      "binding header ABI mismatch");
    if (!module_owned(realm, module) || each == NULL)
        return refuse(runtime, outcome, OS64_JS_BAD_ARGUMENT, "module and callback are required");
    JSModuleDef *m = JS_VALUE_GET_PTR(module->module);
    int count = JS_GetModuleRequestCount(realm->context, m);
    for (int i = 0; i < count && !observe(runtime); i++) {
        JSAtom request = JS_GetModuleRequest(realm->context, m, i);
        const char *text = JS_AtomToCString(realm->context, request);
        JS_FreeAtom(realm->context, request);
        if (text == NULL) {
            discard_exception(realm->context);
            break;
        }
        each(opaque, text);
        JS_FreeCString(realm->context, text);
    }
    return leave(runtime, outcome);
}

JS_PUBLIC os64_js_status_t os64_js_realm_module_run(os64_js_realm_t *realm, const char *caller_abi,
                                            os64_js_module_t *module, os64_js_outcome_t *outcome)
{
    os64_js_runtime_t *runtime = realm != NULL ? realm->runtime : NULL;
    os64_js_status_t result = enter_task(runtime, caller_abi, true, outcome);
    if (result != OS64_JS_OK) return result;
    if (!module_owned(realm, module) || module->run)
        return refuse(runtime, outcome, OS64_JS_BAD_ARGUMENT, "a module is run once");
    if (observe(runtime)) return leave(runtime, outcome);
    module->run = true;
    realm->evaluated = true;
    JSValue promise = JS_EXCEPTION;
    if (JS_ResolveModule(realm->context, module->module) == 0)
        promise = JS_EvalFunction(realm->context, JS_DupValue(realm->context, module->module));
    if (JS_IsException(promise)) {
        // Resolving or linking threw: no evaluation, so no promise.
        JSValue error = JS_GetException(realm->context);
        caught(runtime, realm->context, error);
        outcome->status = OS64_JS_EXCEPTION;
        diagnostic(runtime, realm->context, error, outcome);
        module->rejected = true;
        module->failure = *outcome;
        JS_FreeValue(realm->context, error);
        return leave(runtime, outcome);
    }
    module->promise = promise;
    if (JS_PromiseState(realm->context, promise) == JS_PROMISE_REJECTED) {
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
        JSValue reason = JS_PromiseResult(realm->context, promise);
        module_failed(runtime, realm->context, module, reason, false);
        JS_FreeValue(realm->context, reason);
        *outcome = module->failure;
        outcome->status = OS64_JS_EXCEPTION;
    } else if (JS_PromiseState(realm->context, promise) == JS_PROMISE_PENDING) {
        module_watch(realm, promise);
    }
    return leave(runtime, outcome);
}

JS_PUBLIC os64_js_module_state_t os64_js_realm_module_state(os64_js_realm_t *realm, os64_js_module_t *module,
                                                    os64_js_outcome_t *failure)
{
    os64_js_runtime_t *runtime = realm != NULL ? realm->runtime : NULL;
    if (failure != NULL) os64_memset(failure, 0, sizeof(*failure));
    if (runtime == NULL || !module_owned(realm, module)) {
        if (failure != NULL) failure->status = OS64_JS_BAD_ARGUMENT;
        return OS64_JS_MODULE_UNRUN;
    }
    if (!module->run) return OS64_JS_MODULE_UNRUN;
    if (module->rejected) {
        if (failure != NULL) *failure = module->failure;
        return OS64_JS_MODULE_REJECTED;
    }
    switch (JS_PromiseState(realm->context, module->promise)) {
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
    os64_js_realm_t *realm = module->realm;
    if (module->prev != NULL) module->prev->next = module->next;
    else realm->modules = module->next;
    if (module->next != NULL) module->next->prev = module->prev;
    JS_FreeValueRT(runtime->engine, module->module);
    JS_FreeValueRT(runtime->engine, module->promise);
    js_free_rt(runtime->engine, module);
}

JS_PUBLIC os64_js_status_t os64_js_realm_module_release(os64_js_realm_t *realm, const char *caller_abi,
                                                        os64_js_module_t *module, os64_js_outcome_t *outcome)
{
    os64_js_runtime_t *runtime = realm != NULL ? realm->runtime : NULL;
    if (outcome == NULL) return OS64_JS_BAD_ARGUMENT;
    if (outcome_in_use(runtime, outcome)) return OS64_JS_BUSY;
    os64_memset(outcome, 0, sizeof(*outcome));
    if (runtime == NULL || caller_abi == NULL)
        return status(outcome, OS64_JS_BAD_ARGUMENT, "realm and ABI are required");
    if (os64_strcmp(caller_abi, OS64_JS_ABI_ID) != 0)
        return status(outcome, OS64_JS_ABI_MISMATCH, "binding header ABI mismatch");
    if (runtime->active) return status(outcome, OS64_JS_BUSY, "runtime is executing");
    if (module == NULL) return OS64_JS_OK;
    if (!module_owned(realm, module))
        return status(outcome, OS64_JS_BAD_ARGUMENT, "module belongs to another realm");
    module_free(runtime, module);
    return OS64_JS_OK;
}

JS_PUBLIC void os64_js_module_release(os64_js_runtime_t *runtime, os64_js_module_t *module)
{
    if (runtime == NULL || module == NULL) return;
    os64_js_outcome_t out;
    if (os64_js_realm_module_release(primary(runtime), OS64_JS_ABI_ID, module, &out) != OS64_JS_OK)
        jsport_fatal("module release ownership or active call", __FILE__, __LINE__);
}

JS_PUBLIC void os64_js_destroy_report(os64_js_runtime_t *runtime,
                                      os64_js_teardown_report_t *report)
{
    if (report != NULL) os64_memset(report, 0, sizeof(*report));
    if (runtime == NULL) return;
    if (runtime->active) jsport_fatal("destroy during an active runtime call", __FILE__, __LINE__);
    runtime->active = true;
    runtime->destroying = true;
    runtime->failure = OS64_JS_FAILED_RUNTIME;
    for (size_t i = 0; i < runtime->realm_count; i++) runtime->realms[i].hosted = false;
    runtime->router = (os64_js_rejection_router_t){0};
    JS_SetHostPromiseRejectionTracker(runtime->engine, NULL, NULL);
    clear_rejections(runtime);
    // The host's module handles go before the context they belong to.
    for (size_t i = 0; i < runtime->realm_count; i++) {
        os64_js_realm_t *realm = &runtime->realms[i];
        while (realm->modules != NULL) module_free(runtime, realm->modules);
        JS_FreeValue(realm->context, realm->promise_then);
        realm->promise_then = JS_UNDEFINED;
    }
    for (size_t i = 0; i < runtime->realm_count; i++) JS_FreeContext(runtime->realms[i].context);
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

/* Compatibility entries keep the single-document primary realm. */
JS_PUBLIC os64_js_status_t os64_js_run(os64_js_runtime_t *runtime,
                                     const void *source, size_t length,
                                     const char *source_name,
                                     os64_js_outcome_t *outcome)
{
    return os64_js_realm_run(primary(runtime), source, length, source_name, outcome);
}

JS_PUBLIC os64_js_status_t os64_js_run_bytecode(os64_js_runtime_t *runtime,
                                              const void *source, size_t length,
                                              const char *source_name,
                                              os64_js_bytecode_t *bytecode,
                                              os64_js_outcome_t *outcome)
{
    return os64_js_realm_run_bytecode(primary(runtime), source, length, source_name, bytecode, outcome);
}

JS_PUBLIC os64_js_status_t os64_js_task_begin(os64_js_runtime_t *runtime, const char *caller_abi,
                                            const char *source_name, os64_js_outcome_t *outcome)
{
    return os64_js_realm_task_begin(primary(runtime), caller_abi, source_name, outcome);
}

JS_PUBLIC os64_js_status_t os64_js_call(os64_js_runtime_t *runtime, const char *caller_abi,
                                      JSValue function, JSValue this_value, int argc,
                                      JSValue *argv, bool *returned_false,
                                      os64_js_outcome_t *outcome)
{
    return os64_js_realm_call(primary(runtime), caller_abi, function, this_value, argc, argv, returned_false, outcome);
}

JS_PUBLIC os64_js_status_t os64_js_set_script_host(os64_js_runtime_t *runtime, const char *caller_abi,
                                                 const os64_js_script_host_t *host,
                                                 os64_js_outcome_t *outcome)
{
    if (runtime != NULL && runtime->active) {
        if (outcome == NULL) return OS64_JS_BAD_ARGUMENT;
        if (outcome_in_use(runtime, outcome)) return OS64_JS_BUSY;
        os64_memset(outcome, 0, sizeof(*outcome));
        return status(outcome, OS64_JS_BUSY, "runtime is executing");
    }
    return os64_js_realm_set_script_host(primary(runtime), caller_abi, host, outcome);
}

JS_PUBLIC bool os64_js_take_rejection(os64_js_runtime_t *runtime, const char *caller_abi,
                                     JSValue *promise, JSValue *reason, uint32_t *tag)
{
    return os64_js_realm_take_rejection(primary(runtime), caller_abi, promise, reason, tag);
}

JS_PUBLIC os64_js_status_t os64_js_set_module_loader(os64_js_runtime_t *runtime, const char *caller_abi,
                                                   const os64_js_module_loader_t *loader,
                                                   os64_js_outcome_t *outcome)
{
    os64_js_status_t result = enter(runtime, outcome);
    if (result != OS64_JS_OK) return result;
    /* The realm setter runs no script; release the compatibility guard before
     * delegating, preserving its BUSY/FAILED_RUNTIME entry behavior. */
    refuse(runtime, outcome, OS64_JS_OK, NULL);
    return os64_js_realm_set_module_loader(primary(runtime), caller_abi, loader, outcome);
}

JS_PUBLIC os64_js_status_t os64_js_module_compile(os64_js_runtime_t *runtime, const char *caller_abi,
                                                const void *source, size_t length, const char *name,
                                                const char *url, os64_js_module_t **module,
                                                os64_js_outcome_t *outcome)
{
    return os64_js_realm_module_compile(primary(runtime), caller_abi, source, length, name, url, module, outcome);
}

JS_PUBLIC os64_js_status_t os64_js_module_compile_bytecode(os64_js_runtime_t *runtime, const char *caller_abi,
                                                         const void *source, size_t length, const char *name,
                                                         const char *url, os64_js_bytecode_t *bytecode,
                                                         os64_js_module_t **module, os64_js_outcome_t *outcome)
{
    return os64_js_realm_module_compile_bytecode(primary(runtime), caller_abi, source, length, name, url, bytecode, module, outcome);
}

JS_PUBLIC os64_js_status_t os64_js_module_requests(os64_js_runtime_t *runtime, const char *caller_abi,
                                                 os64_js_module_t *module,
                                                 void (*each)(void *opaque, const char *specifier),
                                                 void *opaque, os64_js_outcome_t *outcome)
{
    return os64_js_realm_module_requests(primary(runtime), caller_abi, module, each, opaque, outcome);
}

JS_PUBLIC os64_js_status_t os64_js_module_run(os64_js_runtime_t *runtime, const char *caller_abi,
                                            os64_js_module_t *module, os64_js_outcome_t *outcome)
{
    return os64_js_realm_module_run(primary(runtime), caller_abi, module, outcome);
}

JS_PUBLIC os64_js_module_state_t os64_js_module_state(os64_js_runtime_t *runtime, os64_js_module_t *module,
                                                    os64_js_outcome_t *failure)
{
    return os64_js_realm_module_state(primary(runtime), module, failure);
}

JS_PUBLIC os64_js_status_t os64_js_install_output(os64_js_runtime_t *runtime,
                                                int32_t handle, uint32_t names,
                                                os64_js_outcome_t *outcome)
{
    return install_output(primary(runtime), handle, names, outcome, false);
}

JS_PUBLIC os64_js_status_t os64_js_realm_install_output(os64_js_realm_t *realm,
                                                int32_t handle, uint32_t names,
                                                os64_js_outcome_t *outcome)
{
    return install_output(realm, handle, names, outcome, true);
}

JS_PUBLIC os64_js_status_t os64_js_install_intl(os64_js_runtime_t *runtime, const char *locale,
                                              os64_js_outcome_t *outcome)
{
    return install_intl(primary(runtime), locale, outcome, false);
}

JS_PUBLIC os64_js_status_t os64_js_realm_install_intl(os64_js_realm_t *realm, const char *locale,
                                              os64_js_outcome_t *outcome)
{
    return install_intl(realm, locale, outcome, true);
}

/* The browser supplies its settings policy here. The default embedding uses
 * the native operation context, which alone is not HTML caller attribution. */
JS_PUBLIC os64_js_status_t os64_js_set_rejection_router(os64_js_runtime_t *runtime, const char *caller_abi,
                                                       const os64_js_rejection_router_t *router,
                                                       os64_js_outcome_t *outcome)
{
    if (outcome == NULL) return OS64_JS_BAD_ARGUMENT;
    if (outcome_in_use(runtime, outcome)) return OS64_JS_BUSY;
    os64_memset(outcome, 0, sizeof(*outcome));
    if (runtime == NULL || caller_abi == NULL || (router != NULL && router->select == NULL))
        return status(outcome, OS64_JS_BAD_ARGUMENT, "runtime, ABI and selector are required");
    if (os64_strcmp(caller_abi, OS64_JS_ABI_ID) != 0)
        return status(outcome, OS64_JS_ABI_MISMATCH, "binding header ABI mismatch");
    if (runtime->active) return status(outcome, OS64_JS_BUSY, "runtime is executing");
    runtime->router = router != NULL ? *router : (os64_js_rejection_router_t){0};
    return OS64_JS_OK;
}
