#ifndef OS64_WEBP_ASSERT_H
#define OS64_WEBP_ASSERT_H
#ifdef WEBP_HOST_ASSERT
#include_next <assert.h>
#else
/* Guest assertions diagnose violated invariants; malformed input uses statuses. */
#define assert(condition) ((condition) ? (void)0 : __builtin_trap())
#endif
#endif
