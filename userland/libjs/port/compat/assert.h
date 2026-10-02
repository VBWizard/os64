#ifndef OS64_JSPORT_ASSERT_H
#define OS64_JSPORT_ASSERT_H
#include "platform.h"
/* Engine invariants remain active in target builds, including release builds. */
#define assert(condition) ((condition) ? (void)0 : jsport_fatal(#condition, __FILE__, __LINE__))
#endif
