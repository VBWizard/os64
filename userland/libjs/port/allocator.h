#ifndef OS64_JSPORT_ALLOCATOR_H
#define OS64_JSPORT_ALLOCATOR_H
#include "quickjs.h"
#include <stdbool.h>
enum { JSPORT_ALLOC_LIMIT = 1, JSPORT_ALLOC_OOM = 2 };
/* When opaque is non-NULL it points to this caller-owned state, which must
 * outlive the runtime. The ceiling applies during construction too. Failure
 * flags accumulate until the owner clears them at an operation boundary. */
typedef union JSPortBlock JSPortBlock;
typedef struct {
    size_t allocation_limit;
    unsigned failures;
    JSPortBlock *blocks;
    size_t live_blocks, live_bytes;
    bool tracked;
} JSPortAllocator;
/* Fatal-profile allocations have no additional header. The tracked profile
 * charges its aligned ledger header along with the usable engine payload. */
extern const JSMallocFunctions jsport_malloc_functions;
extern const JSMallocFunctions jsport_tracked_malloc_functions;
void jsport_allocator_reclaim(JSPortAllocator *owner);
/* Returns 1 at the audited object/weakref leak gate; 0 after clean destroy.
 * A leaking engine is dead and must be reclaimed without engine entry. */
int jsport_free_runtime_report(JSRuntime *runtime);
#endif
