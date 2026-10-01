#ifndef OS64_JS_ENGINE_H
#define OS64_JS_ENGINE_H
#include "os64/js.h"

/* Include quickjs.h for values and bindings. Forward declarations keep the
 * runner independent of the engine's C compatibility headers. */
typedef struct JSContext JSContext;
typedef uint32_t JSClassID;

/* Borrow the runtime-owned context, including inside its native callbacks.
 * No concurrent use, ownership transfer, configuration replacement, or bypass
 * of the top-level re-entry rule. See CONTRACT.md. */
JSContext *os64_js_context(os64_js_runtime_t *runtime);
/* Serialize process-global ID allocation. The caller's slot must itself be
 * synchronized if shared; register the returned ID separately in each runtime. */
JSClassID os64_js_new_class_id(void);
#endif
