#ifndef OS64_JS_ENGINE_H
#define OS64_JS_ENGINE_H
#include "os64/js.h"

/* Include quickjs.h for values and bindings. Forward declarations keep the
 * runner independent of the engine's C compatibility headers. */
typedef struct JSContext JSContext;
typedef struct JSValue JSValue;
typedef uint32_t JSClassID;

typedef enum {
    OS64_JS_SETTINGS_ENTRY,
    OS64_JS_SETTINGS_INCUMBENT,
    OS64_JS_SETTINGS_CURRENT,
    OS64_JS_SETTINGS_NATIVE_CALLER
} os64_js_settings_role_t;
typedef JSValue os64_js_settings_body_t(JSContext *entry, void *opaque);
/* Owner-thread checked scope inside an existing turn/task. Entry and optional
 * captured incumbent must be live, published members of the same runtime;
 * private candidate setup is refused.
 * NULL incumbent selects ordinary entry; non-NULL prepares a callback.
 * Nested native bodies borrow the guard/budget; wrapper run/call stays BUSY.
 * No task opening, checkpoint, deadline reset, exception reporting or capture.
 * Body returns normally in C. Refusal leaves value untouched; admission
 * transfers body's result (including JS_EXCEPTION) through value. Initialize
 * value to JS_UNDEFINED and release it even when a sticky failure is reported.
 * EXCEPTION leaves the engine exception pending for the caller. Every nested
 * invocation needs separate outcome storage. See CONTRACT.md. */
os64_js_status_t os64_js_realm_with_settings(os64_js_realm_t *entry,
    os64_js_realm_t *incumbent, const char *caller_abi, os64_js_settings_body_t *body,
    void *opaque, JSValue *value, os64_js_outcome_t *outcome);
/* Borrow a live realm for this role inside a checked scope. NULL on refusal;
 * no fallback for missing/foreign/retired/installing settings. No allocation,
 * clock read, exception consumption or lifetime extension. Recheck after
 * reentry/retirement. NATIVE_CALLER is the pre-entry realm of the innermost
 * active C-function/data/optimized-iterator invocation, retained across its
 * JS reentry; it is not CURRENT or a generic caller. Unknown custom call
 * classes mask CURRENT and supply no native-caller proof to their callees.
 * This is attribution, not an origin permission check. */
os64_js_realm_t *os64_js_settings_realm(os64_js_runtime_t *runtime,
    const char *caller_abi, os64_js_settings_role_t role, os64_js_outcome_t *outcome);

/* Borrow the primary realm context, including inside native callbacks.
 * The calling unit supplies its compiled-in ABI ID and a separate outcome;
 * mismatch returns NULL/ABI_MISMATCH without exposing the context.
 * No concurrent use, ownership transfer, configuration replacement, or bypass
 * of the top-level re-entry rule. Runtime and context opaque slots belong to
 * the wrapper; bindings keep state in their own objects or function data.
 * See CONTRACT.md. */
JSContext *os64_js_context(os64_js_runtime_t *runtime, const char *caller_abi,
                           os64_js_outcome_t *outcome);
/* Serialize the slot check and process-global ID allocation together. A
 * zero-initialized process-lifetime slot receives one ID, reused on later
 * calls. Access the shared slot through this function; register the returned
 * ID separately in each runtime. NULL returns zero without engine entry. */
JSClassID os64_js_class_id(JSClassID *slot);

/* A host TASK: one execution deadline and one job cap, armed by task_begin,
 * shared by every call and checkpoint until task_end. One task is open at a
 * time; begin is BUSY while one is open or a turn still has runnable jobs.
 * Rejections are judged only at checkpoints (and task_end's final one), or
 * left for a script host to take (os64_js_set_script_host, below). A
 * sticky failure inside any entry ends the task's work; task_end still
 * closes it and reports the sticky status. See CONTRACT.md § Host tasks. */
os64_js_status_t os64_js_task_begin(os64_js_runtime_t *runtime, const char *caller_abi,
                                    const char *source_name, os64_js_outcome_t *outcome);
/* Calls a function value inside the open task, with eval's outcomes. The
 * host owns function, this and argv. The return value is discarded; a
 * strict `false` is reported through returned_false (may be NULL), which is
 * what an event handler's `return false` needs. The function realm must also
 * remain eligible; choosing a live context does not revive a retired callback.
 * Refused outside a task and
 * from inside a callback; a binding nested under a call uses JS_Call. */
os64_js_status_t os64_js_call(os64_js_runtime_t *runtime, const char *caller_abi,
                              JSValue function, JSValue this_value, int argc,
                              JSValue *argv, bool *returned_false,
                              os64_js_outcome_t *outcome);
/* A microtask checkpoint inside the open task: drains runnable jobs under
 * the task's deadline and cap, reporting the first job exception and
 * carrying on, then judges unhandled rejections (without a script host).
 * With no job exception, its outcome may be a module's late failure
 * (CONTRACT.md § Modules: owed to the call that surfaced it). */
os64_js_status_t os64_js_checkpoint(os64_js_runtime_t *runtime, const char *caller_abi,
                                    os64_js_outcome_t *outcome);
/* A final checkpoint, then the task is closed and eval/run are accepted;
 * its outcome may be a module's late failure, as a checkpoint's may. */
os64_js_status_t os64_js_task_end(os64_js_runtime_t *runtime, const char *caller_abi,
                                  os64_js_outcome_t *outcome);
/* Whether a job queued now (JS_EnqueueJob) will be drained by the turn
 * under way: true inside a call, an eval or a run, and while a task is
 * open. Outside all of them a queued job is left for nobody, and the next
 * task_begin answers BUSY until something drains it, so a binding told of
 * a change by its host (a parser's report, between tasks) asks first and
 * defers the work another way when the answer is no. No engine entry. */
bool os64_js_turn_open(const os64_js_runtime_t *runtime);
/* Replaces the execution limit for tasks and turns started from now on; an
 * open task keeps the deadline it was armed with. Owner thread, outside a
 * call. Zero or unrepresentable is BAD_ARGUMENT, as creation refuses. */
os64_js_status_t os64_js_set_execution_ms(os64_js_runtime_t *runtime, uint64_t ms,
                                          os64_js_outcome_t *outcome);
/* The most the runtime's heap has held since it was made, in bytes: the
 * engine's charge, which its memory limit is held against. Readable from
 * a runtime that has failed. 0 for NULL. No engine entry. */
size_t os64_js_heap_peak(const os64_js_runtime_t *runtime);
/* What the heap holds now, in the same bytes. Readable from a runtime that
 * has failed. 0 for NULL. No engine entry. */
size_t os64_js_heap_bytes(const os64_js_runtime_t *runtime);
/* What the runtime's cycle collections have cost since it was made, counted
 * by the engine at every run whether or not the ledger runs: runs, and of
 * them those the allocation threshold asked for; the GC objects they walked
 * and freed from cycles, and the most one run walked; the heap's bytes
 * before and after each run, summed (the engine's own count, which the
 * memory limit is not held against); the threshold the next
 * allocation-asked run waits for; and their time on os64_js_ledger_now()'s
 * clock, with the longest. Zeroed for NULL or a runtime whose engine is
 * gone. No engine entry. */
typedef struct {
    uint64_t runs, runs_by_allocation;
    uint64_t walked, freed, walked_most;
    uint64_t bytes_before, bytes_after, threshold;
    uint64_t ticks, ticks_most;
} os64_js_collector_t;
void os64_js_collector(const os64_js_runtime_t *runtime, os64_js_collector_t *out);

/* THE LEDGER: where the runtime's script time goes, for measuring a page
 * (JS_SPEED measurement; runtime/ledger.h holds the arithmetic). Off unless
 * enabled, when it costs the engine one pointer test at each native call,
 * call into script from C and collection. On, each of those is a frame
 * timed by os64_js_ledger_now()'s clock, as are the runtime's own: ENGINE
 * from a call making the runtime active (a run, an eval, a call, a task, a
 * drain of jobs) to its end, and COMPILE around each compile or bytecode
 * read the runtime makes. A host may add a frame of its own with push and
 * pop, which must pair inside one call into the host (LAYOUT around a
 * layout script forced). A frame's own time (its elapsed less its
 * children's) is charged to its kind, so the kinds add up to the outermost
 * frames' elapsed, and a frame open means script is running.
 *   ENGINE     the runtime's own time: the interpreter, the engine's C
 *              builtins (sort, regular expressions, JSON, string work),
 *              host functions made without data (JS_NewCFunction) and
 *              libjs's own bookkeeping, none of which passes a door;
 *   NATIVE     a host function made with data (JS_NewCFunctionData:
 *              libdom's members, and a few of libjs's own), also charged by
 *              callee (function, magic) with the layouts it forced. Script
 *              a host call runs through JS_Call, JS_CallConstructor(2) or
 *              JS_EvalFunction is ENGINE again; script it reaches another
 *              way (a getter, a conversion, an iterator) stays in its time.
 * With OS64_JS_LEDGER_PROFILE the interrupt handler, which the engine calls
 * when it polls (every so many operations of the interpreter, regular
 * expression matcher and a few long builtins), samples the running
 * function and its caller at most once every `sample_ticks` of the clock,
 * charging each sample the ENGINE time since the previous one and never
 * time inside another kind; a sample taken outside bytecode names
 * "[native]". Everything here is owner thread and enters no engine code. */
typedef enum {
    OS64_JS_LEDGER_ENGINE,
    OS64_JS_LEDGER_COMPILE,
    OS64_JS_LEDGER_COLLECTOR,
    OS64_JS_LEDGER_NATIVE,
    OS64_JS_LEDGER_LAYOUT,
    OS64_JS_LEDGER_KINDS
} os64_js_ledger_kind_t;
#define OS64_JS_LEDGER_PROFILE 0x1u
typedef struct {
    uint64_t self[OS64_JS_LEDGER_KINDS];   /* own time, in the clock's ticks */
    uint64_t count[OS64_JS_LEDGER_KINDS];  /* frames closed */
    uint64_t frames_lost, callees_lost;    /* nested too deep; past a full callee table */
    uint64_t samples, samples_lost;        /* profile samples taken; past a full table */
    unsigned open;                         /* frames open now */
    uint64_t since;                        /* the clock when it was enabled or last reset */
    uint64_t resets;                       /* resets since it was enabled */
} os64_js_ledger_totals_t;
typedef struct {
    const void *function;                  /* the host function, as registered */
    int magic;
    uint64_t calls, self, forced, forced_ticks;
} os64_js_ledger_callee_t;
typedef struct {
    /* Compared, never followed, and with the place below: a freed function's
     * address can be another's later. NULL for a native frame. */
    const void *function;
    char name[64];
    char file[128];
    int line, column;                      /* where the function is defined */
} os64_js_ledger_place_t;
typedef struct {
    os64_js_ledger_place_t at, caller;
    uint64_t samples, ticks;               /* interpreter time charged */
} os64_js_ledger_sample_t;
/* The ledger's clock: the CPU's time-stamp counter. */
uint64_t os64_js_ledger_now(void);
/* Starts the ledger (zeroed), or changes its flags and sample period if it
 * runs. Started only between calls into the runtime: BUSY from inside one.
 * HOST_FAILURE when its memory (the host's, not the engine's heap) cannot be
 * had; BAD_ARGUMENT for NULL, an unknown flag, or PROFILE with a zero
 * period. */
os64_js_status_t os64_js_ledger_enable(os64_js_runtime_t *runtime, unsigned flags, uint64_t sample_ticks);
/* Stops and frees it, frames still open with it; a bracket the engine
 * opened before is closed into nothing. */
void os64_js_ledger_disable(os64_js_runtime_t *runtime);
/* Zeroes the totals, rows and samples; frames open stay open from now. */
void os64_js_ledger_reset(os64_js_runtime_t *runtime);
/* A host's frame, of kind ENGINE or LAYOUT, paired inside one call into
 * the host (a pop must close the host's own push, never a frame the engine
 * or the runtime opened); nothing when the ledger is off. */
void os64_js_ledger_push(os64_js_runtime_t *runtime, os64_js_ledger_kind_t kind);
void os64_js_ledger_pop(os64_js_runtime_t *runtime);

/* The totals; false (and zeroed) when the ledger is off. */
bool os64_js_ledger_totals(const os64_js_runtime_t *runtime, os64_js_ledger_totals_t *out);
/* The next callee row or sample after *cursor (start it at 0); false at
 * the end or when the ledger is off. Rows come in table order. */
bool os64_js_ledger_callee(const os64_js_runtime_t *runtime, size_t *cursor, os64_js_ledger_callee_t *out);
bool os64_js_ledger_sample(const os64_js_runtime_t *runtime, size_t *cursor, os64_js_ledger_sample_t *out);
/* Remaining engine allowance after the runtime wrapper's storage and current
 * allocator charge. Owner thread, including callbacks; no allocation, GC or
 * failure observation. Readable after failure, but not evidence of health.
 * Returns 0 for NULL or exhausted allowance. */
size_t os64_js_heap_headroom(const os64_js_runtime_t *runtime);
/* THE HEAP BY KIND OF THING (MEMORY_CENSUS.md § 5): the engine's own count
 * of what it holds. It walks every object, string, shape and function the
 * heap has, so it costs time in proportion to the heap: for a census, not
 * for every turn. Bytes are the engine's estimates of each kind's own
 * storage; `charged_bytes` is os64_js_heap_bytes, which also pays for what
 * no kind claims (the allocator's headers, the engine's tables). */
typedef struct {
    uint64_t objects, object_bytes, properties, property_bytes;
    uint64_t strings, string_bytes, atoms, atom_bytes;
    uint64_t shapes, shape_bytes;
    uint64_t functions, function_bytes, function_code_bytes;
    uint64_t arrays, fast_array_elements;
    uint64_t binary_objects, binary_bytes;
    uint64_t blocks;
    size_t charged_bytes;
} os64_js_heap_census_t;
/* Owner thread, outside a call. False, and `out` zeroed, for NULL, a
 * runtime that has failed, or one inside a call. Runs no script. */
bool os64_js_heap_census(os64_js_runtime_t *runtime, os64_js_heap_census_t *out);
/* A SCRIPT HOST: a host that reports a script's failures itself, as a
 * browser fires `error` and `unhandledrejection` (CONTRACT.md § A script
 * host). Each hook is called inside the wrapper's guard and must run no
 * script; it may keep the value (JS_DupValue) to act on after the call.
 *   exception: every exception eval, run, a call, a job drain or a
 *     checkpoint catches, before its outcome is written; never one a
 *     sticky failure threw.
 *   rejected: a promise was rejected with no handler and the runtime now
 *     holds it, asked at once, while the code that rejected it may be on
 *     the stack (JS_GetScriptOrModuleName); its answer is kept with the
 *     rejection and handed back by os64_js_take_rejection.
 *   handled: a handler was added to a rejected promise the runtime no
 *     longer holds, which is one the host took (one the runtime could not
 *     hold, its memory refused, failed the runtime, so nothing more is
 *     heard of it).
 * With a host set, unhandled rejections are not judged at checkpoints:
 * they wait, oldest first, for os64_js_take_rejection. NULL clears the
 * host, which is accepted from a failed runtime too; BUSY during a call. */
typedef struct {
    void *opaque;
    void (*exception)(void *opaque, JSContext *context, JSValue error);
    void (*handled)(void *opaque, JSContext *context, JSValue promise);
    uint32_t (*rejected)(void *opaque, JSContext *context, JSValue promise);
} os64_js_script_host_t;
os64_js_status_t os64_js_set_script_host(os64_js_runtime_t *runtime, const char *caller_abi,
                                         const os64_js_script_host_t *host, os64_js_outcome_t *outcome);
/* The oldest rejection still unhandled, its promise and reason handed over
 * as new references, and the host's `rejected` answer for it in *tag (may
 * be NULL; 0 without that hook). False with both JS_UNDEFINED when there is
 * none, and when one may not be taken now: no script host, inside a call, a
 * failed runtime, or jobs still runnable (a rejection is unhandled only
 * once the microtask checkpoint that could have handled it has run). */
bool os64_js_take_rejection(os64_js_runtime_t *runtime, const char *caller_abi,
                            JSValue *promise, JSValue *reason, uint32_t *tag);
/* ── ES MODULES (CONTRACT.md § Modules) ──────────────────────────────────
 * A module is compiled from its source under its NAME, its URL: what its
 * imports are resolved against and what import.meta.url says. It is
 * compiled UNRESOLVED, so a host that must fetch its imports first reads
 * them (os64_js_module_requests), compiles each as it arrives, and runs the
 * entry once every module of the graph is compiled: run finds each import
 * by name among the modules compiled so far, asks the loader for any it
 * cannot find, links, and evaluates. */
typedef struct os64_js_module os64_js_module_t;

/* The host's two answers, both on the owner thread, inside a call (they
 * must run no script):
 *   resolve: the name `specifier` means in the module named `base`,
 *     os64_malloc'd, or NULL when it names nothing (the import throws).
 *   source: the source of the module named `name`, borrowed until the call
 *     returns; false when there is none (the import throws). Asked for a
 *     dynamic import(), and for a module of a graph the engine let go of
 *     (a failed run unlists the modules no run has resolved); it may fetch.
 * A NULL loader, or a NULL member, throws for every import it would answer. */
typedef struct {
    void *opaque;
    char *(*resolve)(void *opaque, const char *base, const char *specifier);
    bool (*source)(void *opaque, const char *name, const char **bytes, size_t *length);
} os64_js_module_loader_t;
os64_js_status_t os64_js_set_module_loader(os64_js_runtime_t *runtime, const char *caller_abi,
                                           const os64_js_module_loader_t *loader,
                                           os64_js_outcome_t *outcome);
/* Compiles `source` (borrowed) as the module named `name`, unresolved, and
 * hands back the host's handle. `url` is its import.meta.url, NULL for its
 * name: an inline module script is the document's URL there, while each
 * needs a name of its own, since the engine finds modules by name; compile
 * each name once (a second module of one name is never found). Outside a
 * call, task open or not: it runs no script. EXCEPTION for a syntax error,
 * its diagnostic in the outcome alone (not the script host's: no task is
 * running) and *module NULL; LIMIT for a source past the limit. */
os64_js_status_t os64_js_module_compile(os64_js_runtime_t *runtime, const char *caller_abi,
                                        const void *source, size_t length, const char *name,
                                        const char *url, os64_js_module_t **module,
                                        os64_js_outcome_t *outcome);
/* os64_js_module_compile through `bytecode` (os64/js.h; BYTECODE_CACHE.md
 * § 9): the module read from its blob when the blob was written for a module
 * of this same `name` — the name is a module's identity, and a read cannot
 * give it another — else compiled, its blob kept when asked (a read keeps
 * none: nothing was compiled). The source is always required: a refused
 * blob means the source is compiled. A NULL `bytecode` is BAD_ARGUMENT. */
os64_js_status_t os64_js_module_compile_bytecode(os64_js_runtime_t *runtime, const char *caller_abi,
                                                 const void *source, size_t length, const char *name,
                                                 const char *url, os64_js_bytecode_t *bytecode,
                                                 os64_js_module_t **module, os64_js_outcome_t *outcome);
/* Each static import specifier of `module` as written, in source order, to
 * `each` (the string borrowed for the call; `each` may not enter the
 * runtime). Outside a call. */
os64_js_status_t os64_js_module_requests(os64_js_runtime_t *runtime, const char *caller_abi,
                                         os64_js_module_t *module,
                                         void (*each)(void *opaque, const char *specifier),
                                         void *opaque, os64_js_outcome_t *outcome);
/* Inside an open task, once: resolves, links and evaluates `module`. OK
 * with the evaluation begun (os64_js_module_state says how it went);
 * EXCEPTION when resolving or linking threw (an import nobody could load,
 * an export that is not there), or when the evaluation failed at once.
 * Either failure goes to the script host's `exception`, never to
 * `rejected`. An evaluation still pending is watched: when it fails later,
 * the host's `exception` hears it then, and the outcome of the call whose
 * jobs surfaced it carries it (CONTRACT.md § Modules). */
os64_js_status_t os64_js_module_run(os64_js_runtime_t *runtime, const char *caller_abi,
                                    os64_js_module_t *module, os64_js_outcome_t *outcome);
typedef enum {
    OS64_JS_MODULE_UNRUN = 0,
    OS64_JS_MODULE_PENDING,         /* a top-level await has not finished */
    OS64_JS_MODULE_FULFILLED,
    OS64_JS_MODULE_REJECTED
} os64_js_module_state_t;
/* How `module`'s evaluation stands, asked after each turn's drain: an
 * evaluation that awaits settles whenever what it awaits arrives. With
 * REJECTED and a non-NULL `failure`, the failure's outcome is copied
 * there. Runs no script; owner thread, outside a call. */
os64_js_module_state_t os64_js_module_state(os64_js_runtime_t *runtime, os64_js_module_t *module,
                                            os64_js_outcome_t *failure);
/* The handle let go (the module stays the engine's while anything imports
 * it). Outside a call; NULL is a no-op. Handles still held are let go by
 * destroy. */
void os64_js_module_release(os64_js_runtime_t *runtime, os64_js_module_t *module);

/* Native-callback budget boundary: uses a separate caller-owned outcome.
 * Requires an active budgeted call; does not enter JS, drain jobs, reset a
 * deadline or release the outer call's guard. Observed failures remain sticky.
 * Native work cannot be preempted here; check before and after bounded work. */
os64_js_status_t os64_js_check_budget(os64_js_runtime_t *runtime, const char *caller_abi,
                                      os64_js_outcome_t *outcome);
/* Multiple realms share one engine, queue, owner thread and task budget.
 * Handles/contexts are borrowed until runtime destruction, including after
 * realm retirement. Legacy operations select the primary realm.
 * Realm creation and initial installers may borrow an active task's guard;
 * run/call do not permit wrapper re-entry. Each handle must come from this
 * library and remain live; arbitrary/stale native pointers are not validated.
 * Realm verbs have their legacy counterpart's contract except for selection
 * and the setup exception. Explicit realm service setters also permit
 * non-executing configuration after failure. See CONTRACT.md § Realms. */
typedef struct {
    void *opaque;
    /* Called without script execution while the triggering stack is present.
     * NULL suppresses a new report. A foreign realm fails the runtime.
     * Handling a still-pending promise removes its record before selection.
     * The browser's settings-policy bridge may extend this seam. */
    os64_js_realm_t *(*select)(void *opaque, JSContext *context, JSValue promise, bool handled);
} os64_js_rejection_router_t;

JSContext *os64_js_realm_context(os64_js_realm_t *realm, const char *caller_abi,
    os64_js_outcome_t *outcome);

os64_js_realm_t *os64_js_primary_realm(os64_js_runtime_t *runtime, const char *caller_abi,
    os64_js_outcome_t *outcome);

os64_js_realm_t *os64_js_realm_from_context(os64_js_runtime_t *runtime, JSContext *context,
    const char *caller_abi, os64_js_outcome_t *outcome);

/* Returns no handle on failure. REALM_LIMIT is non-sticky and attempts no
 * allocation; other resource failures affect the shared runtime. */
os64_js_status_t os64_js_realm_create(os64_js_runtime_t *runtime, const char *caller_abi,
    os64_js_realm_t **out, os64_js_outcome_t *outcome);

/* Irreversible, idempotent owner-thread eligibility mark. Allowed inside a
 * native callback with a separate outcome, and after a sticky failure for
 * cleanup. No allocation, queue mutation, context destruction or cancellation.
 * Existing stacks may unwind; exported ordinary functions remain callable
 * through raw JS_Call under a live caller's budget. New realm entry is refused.
 * Hosts keep provider records alive through active calls and checkpoints, then
 * clear script/module hooks at their safe point. This is not DOM departure. */
os64_js_status_t os64_js_realm_retire(os64_js_realm_t *realm, const char *caller_abi,
    os64_js_outcome_t *outcome);
/* Borrowed handle lifetime is unchanged; false for NULL. No engine entry. */
bool os64_js_realm_is_retired(const os64_js_realm_t *realm);

/* Owner-thread, allocation-free safe-point snapshot. OK means the runtime is
 * healthy with no active call, open task/turn, installer, checkpoint or queued
 * microtask or finalization task. BUSY preserves those operations; FAILED_RUNTIME requires root cleanup.
 * Does not run jobs, sample time, reset budgets, latch failures or detach hosts.
 * An aliased active outcome is refused without writing it. Native bindings
 * must also exclude their own dispatch/reaction/provider borrows, and must
 * not enter engine work between this check and the protected cleanup. */
os64_js_status_t os64_js_quiescent(os64_js_runtime_t *runtime, const char *caller_abi,
    os64_js_outcome_t *outcome);

/* Owner-thread admission snapshot, including inside a parent's callback with
 * queued jobs. Checks slots and a positive caller-measured installation peak
 * plus sibling headroom; does not allocate, execute jobs, change the enclosing
 * budget/outcome or reserve bytes. REALM_LIMIT and ADMISSION_REFUSED are local.
 * Recheck at creation: intervening allocations can invalidate an OK snapshot.
 * This does not guarantee later allocations or system memory availability. */
os64_js_status_t os64_js_realm_preflight(os64_js_runtime_t *runtime, const char *caller_abi,
    size_t required_headroom, os64_js_outcome_t *outcome);
/* Rechecks preflight immediately before the normal realm creation path.
 * Refusal publishes no handle and consumes no slot. Creation/installation
 * allocator failures retain the shared runtime's existing sticky behavior.
 * Hosts choose the reserve for their bindings; this creates the engine realm
 * only, and does not install or publish DOM services. */
os64_js_status_t os64_js_realm_create_with_headroom(os64_js_runtime_t *runtime,
    const char *caller_abi, size_t required_headroom,
    os64_js_realm_t **out, os64_js_outcome_t *outcome);

/* Private installation: stage consumes a stable slot but refuses ordinary
 * execution/host delivery. Keep its objects unexposed until activation.
 * Setup invokes trusted native installation/private bootstrap only; it may
 * not call siblings, external providers, or publish candidate references.
 * Return OK, ADMISSION_REFUSED (local native refusal), or EXCEPTION with a
 * pending engine exception. Engine resource failures remain runtime-wide.
 * Setup borrows an enclosing turn's deadline/job count and preserves sibling
 * queues; standalone setup requires no prior pending turn. Candidate microtasks
 * drain before success; deferred registry cleanup waits for activation and host
 * task selection. Failure discards both candidate lanes, including neutral jobs.
 * Refusal before guard entry leaves the candidate unchanged; failure
 * after entry retires its slot. Native backing cleanup belongs to the caller.
 * Installers use separate outcomes for nested API calls. Activate enables ordinary entry after successful setup; it does
 * not open a new budget or checkpoint siblings. */
typedef os64_js_status_t (*os64_js_realm_setup_fn)(os64_js_realm_t *realm,
    JSContext *context, void *opaque);
os64_js_status_t os64_js_realm_stage(os64_js_runtime_t *runtime, const char *caller_abi,
    size_t required_headroom, os64_js_realm_t **out, os64_js_outcome_t *outcome);
os64_js_status_t os64_js_realm_setup(os64_js_realm_t *realm, const char *caller_abi,
    os64_js_realm_setup_fn installer, void *opaque, os64_js_outcome_t *outcome);
os64_js_status_t os64_js_realm_activate(os64_js_realm_t *realm, const char *caller_abi,
    os64_js_outcome_t *outcome);

os64_js_status_t os64_js_realm_run(os64_js_realm_t *realm,
                                     const void *source, size_t length,
                                     const char *source_name,
                                     os64_js_outcome_t *outcome);

os64_js_status_t os64_js_realm_run_bytecode(os64_js_realm_t *realm,
    const void *source, size_t length,
    const char *source_name,
    os64_js_bytecode_t *bytecode,
    os64_js_outcome_t *outcome);

os64_js_status_t os64_js_realm_task_begin(os64_js_realm_t *realm, const char *caller_abi,
    const char *source_name, os64_js_outcome_t *outcome);

os64_js_status_t os64_js_realm_call(os64_js_realm_t *realm, const char *caller_abi,
    JSValue function, JSValue this_value, int argc,
    JSValue *argv, bool *returned_false,
    os64_js_outcome_t *outcome);

/* NULL may detach a failed child's hooks from its fenced recovery prepare
 * callback. Healthy siblings and other active phases retain BUSY. */
os64_js_status_t os64_js_realm_set_script_host(os64_js_realm_t *realm, const char *caller_abi,
    const os64_js_script_host_t *host,
    os64_js_outcome_t *outcome);

bool os64_js_realm_take_rejection(os64_js_realm_t *realm, const char *caller_abi,
                                     JSValue *promise, JSValue *reason, uint32_t *tag);

/* The same failed-child PREPARING exception permits NULL-only native loader
 * detachment; it does not allow installing providers or changing siblings. */
os64_js_status_t os64_js_realm_set_module_loader(os64_js_realm_t *realm, const char *caller_abi,
    const os64_js_module_loader_t *loader,
    os64_js_outcome_t *outcome);

os64_js_status_t os64_js_realm_module_compile(os64_js_realm_t *realm, const char *caller_abi,
    const void *source, size_t length, const char *name,
    const char *url, os64_js_module_t **module,
    os64_js_outcome_t *outcome);

os64_js_status_t os64_js_realm_module_compile_bytecode(os64_js_realm_t *realm, const char *caller_abi,
    const void *source, size_t length, const char *name,
    const char *url, os64_js_bytecode_t *bytecode,
    os64_js_module_t **module, os64_js_outcome_t *outcome);

os64_js_status_t os64_js_realm_module_requests(os64_js_realm_t *realm, const char *caller_abi,
    os64_js_module_t *module,
    void (*each)(void *opaque, const char *specifier),
    void *opaque, os64_js_outcome_t *outcome);

os64_js_status_t os64_js_realm_module_run(os64_js_realm_t *realm, const char *caller_abi,
    os64_js_module_t *module, os64_js_outcome_t *outcome);

os64_js_module_state_t os64_js_realm_module_state(os64_js_realm_t *realm, os64_js_module_t *module,
    os64_js_outcome_t *failure);

/* BAD_ARGUMENT for a handle owned by another realm; usable after a sticky
 * failure. NULL module succeeds. Legacy release retains fatal misuse checks. */
os64_js_status_t os64_js_realm_module_release(os64_js_realm_t *realm, const char *caller_abi,
    os64_js_module_t *module, os64_js_outcome_t *outcome);

os64_js_status_t os64_js_realm_install_output(os64_js_realm_t *realm,
    int32_t handle, uint32_t names,
    os64_js_outcome_t *outcome);

os64_js_status_t os64_js_realm_install_intl(os64_js_realm_t *realm, const char *locale,
    os64_js_outcome_t *outcome);

os64_js_status_t os64_js_set_rejection_router(os64_js_runtime_t *runtime, const char *caller_abi,
    const os64_js_rejection_router_t *router,
    os64_js_outcome_t *outcome);
#endif
