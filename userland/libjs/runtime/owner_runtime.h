#ifndef JS_OWNER_RUNTIME_H
#define JS_OWNER_RUNTIME_H
#include "owner_incarnation.h"

/* Private numeric inspection. The caller supplies a live runtime on its
 * owner thread, including synchronous finalizer unwind before destroy frees
 * the wrapper. NULL yields zero. A copied number keeps no storage alive and
 * grants no execution, target/security or reclamation authority. */
uint64_t js_owner_runtime_incarnation(const os64_js_runtime_t *);
#endif
