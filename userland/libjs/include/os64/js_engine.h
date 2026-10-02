#ifndef OS64_JS_ENGINE_H
#define OS64_JS_ENGINE_H
#include "os64/js.h"

/* Include quickjs.h for values and bindings. Forward declarations keep the
 * runner independent of the engine's C compatibility headers. */
typedef struct JSContext JSContext;
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
#endif
