#ifndef OS64_JSPORT_ALLOCATOR_H
#define OS64_JSPORT_ALLOCATOR_H
#include "quickjs.h"
enum { JSPORT_ALLOC_LIMIT = 1, JSPORT_ALLOC_OOM = 2 };
/* When opaque is non-NULL it points to this caller-owned state, which must
 * outlive the runtime. The ceiling applies during construction too. Failure
 * flags accumulate until the owner clears them at an operation boundary. */
typedef struct { size_t payload_limit; unsigned failures; } JSPortAllocator;
/* Counts libos64's usable payload, without a second allocation header. */
extern const JSMallocFunctions jsport_malloc_functions;
#endif
