#ifndef JS_OWNER_INCARNATION_H
#define JS_OWNER_INCARNATION_H
#include "owner_tickets.h"

/* Private issuer state: zero-initialize once, keep alive and never copy/reset
 * during its lifetime. Access _last only through the atomic issuer operation.
 * Independent test issuers do not define production identity namespaces. */
typedef struct { uint64_t _last; } JSOwnerIncarnationIssuer;
/* Out must be outside issuer storage. Refusal leaves both unchanged. This
 * issues numbers, not runtime/owner/execution or reclamation authority. */
JSOwnerStatus js_owner_incarnation_reserve(JSOwnerIncarnationIssuer *, uint64_t *out);
/* Shared namespace for this loaded libjs instance in one process. No reset or
 * uncharge: failed later admissions burn their issued identity. Tokens cannot
 * cross processes or library unload/reload lifetimes. */
JSOwnerStatus js_owner_incarnation_next(uint64_t *out);
#endif
