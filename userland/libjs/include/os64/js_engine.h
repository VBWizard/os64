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
 * Rejections are judged only at checkpoints (and task_end's final one). A
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
 * carrying on, then judges unhandled rejections. */
os64_js_status_t os64_js_checkpoint(os64_js_runtime_t *runtime, const char *caller_abi,
                                    os64_js_outcome_t *outcome);
/* A final checkpoint, then the task is closed and eval/run are accepted. */
os64_js_status_t os64_js_task_end(os64_js_runtime_t *runtime, const char *caller_abi,
                                  os64_js_outcome_t *outcome);
/* Replaces the execution limit for tasks and turns started from now on; an
 * open task keeps the deadline it was armed with. Owner thread, outside a
 * call. Zero or unrepresentable is BAD_ARGUMENT, as creation refuses. */
os64_js_status_t os64_js_set_execution_ms(os64_js_runtime_t *runtime, uint64_t ms,
                                          os64_js_outcome_t *outcome);
/* Native-callback budget boundary: uses a separate caller-owned outcome.
 * Requires an active budgeted call; does not enter JS, drain jobs, reset a
 * deadline or release the outer call's guard. Observed failures remain sticky.
 * Native work cannot be preempted here; check before and after bounded work. */
os64_js_status_t os64_js_check_budget(os64_js_runtime_t *runtime, const char *caller_abi,
                                      os64_js_outcome_t *outcome);
#endif
