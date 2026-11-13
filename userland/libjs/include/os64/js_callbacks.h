#ifndef OS64_JS_CALLBACKS_H
#define OS64_JS_CALLBACKS_H
#include "os64/js_engine.h"
#include "quickjs.h"

/* Native-owned callback reference and conversion-time incumbent. Initialize
 * with OS64_JS_CALLBACK_INIT; do not shallow-copy a populated record. Its
 * runtime must outlive the record. C roots clear before runtime destruction;
 * engine-owned holders trace with callback_mark and clear in their finalizer.
 * The holder accounts for this inline storage in its own memory budget. */
typedef struct {
    JSValue value;
    os64_js_runtime_t *runtime;
    os64_js_realm_t *incumbent;
} os64_js_callback_t;
#define OS64_JS_CALLBACK_INIT { JS_UNDEFINED, NULL, NULL }

/* Capture after the binding's callback-type validation, at conversion time.
 * Requires an object, an empty record and a checked incumbent scope. Does not
 * inspect properties, derive a function/entry realm, run script or allocate.
 * Success retains value; refusal leaves the record and exception untouched. */
os64_js_status_t os64_js_callback_capture(os64_js_runtime_t *runtime,
    const char *caller_abi, JSValueConst value, os64_js_callback_t *record,
    os64_js_outcome_t *outcome);
/* Clear before releasing the held value, including in a callback/finalizer
 * and after failure or retirement. Empty records are idempotent. No clock,
 * exception consumption or guard acquisition; normal value finalization runs. */
os64_js_status_t os64_js_callback_clear(os64_js_runtime_t *runtime,
    const char *caller_abi, os64_js_callback_t *record, os64_js_outcome_t *outcome);
/* Call from the owning engine object's gc_mark, for an engine-held record.
 * A C-root record must not be presented as an engine edge. */
void os64_js_callback_mark(JSRuntime *engine, const os64_js_callback_t *record,
    JS_MarkFunc *mark);

typedef JSValue os64_js_callback_body_t(JSContext *entry, JSValueConst callback, void *opaque);
/* The binding supplies the callback's separately established associated entry
 * realm and performs its invocation/conversions in body. This helper does not
 * infer that realm or implement a WebIDL callback type. The value and captured
 * incumbent are snapshotted for this invocation: body may clear/reuse record.
 * The held value is released before leaving the checked settings guard.
 * Return/result/exception rules are those of os64_js_realm_with_settings. */
os64_js_status_t os64_js_callback_with_settings(os64_js_realm_t *entry,
    const char *caller_abi, const os64_js_callback_t *record,
    os64_js_callback_body_t *body, void *opaque, JSValue *value,
    os64_js_outcome_t *outcome);
#endif
