#ifndef OS64_JS_ENGINE_H
#define OS64_JS_ENGINE_H
#include "os64/js.h"

/* Include quickjs.h for values and bindings. Forward declarations keep the
 * runner independent of the engine's C compatibility headers. */
typedef struct JSContext JSContext;
typedef struct JSValue JSValue;
typedef uint32_t JSClassID;

/* Borrow the runtime-owned context, including inside its native callbacks.
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
 * what an event handler's `return false` needs. Refused outside a task and
 * from inside a callback; a binding nested under a call uses JS_Call. */
os64_js_status_t os64_js_call(os64_js_runtime_t *runtime, const char *caller_abi,
                              JSValue function, JSValue this_value, int argc,
                              JSValue *argv, bool *returned_false,
                              os64_js_outcome_t *outcome);
/* A microtask checkpoint inside the open task: drains runnable jobs under
 * the task's deadline and cap, reporting the first job exception and
 * carrying on, then judges unhandled rejections (without a script host). */
os64_js_status_t os64_js_checkpoint(os64_js_runtime_t *runtime, const char *caller_abi,
                                    os64_js_outcome_t *outcome);
/* A final checkpoint, then the task is closed and eval/run are accepted. */
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
 * `rejected`. */
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
#endif
