#ifndef OS64_AV_INTTYPES_H
#define OS64_AV_INTTYPES_H
/* Private to the FFmpeg port. libos64's formatter takes l for 64-bit. */
#include <stdint.h>
#define PRId8 "d"
#define PRIi8 "i"
#define PRIu8 "u"
#define PRIx8 "x"
#define PRIX8 "X"
#define PRIi16 "i"
#define PRIX16 "X"
#define PRIi32 "i"
#define PRIuPTR "lu"
#define PRId16 "d"
#define PRIu16 "u"
#define PRIx16 "x"
#define PRId32 "d"
#define PRIu32 "u"
#define PRIx32 "x"
#define PRIX32 "X"
#define PRId64 "ld"
#define PRIu64 "lu"
#define PRIx64 "lx"
#define PRIX64 "lX"
#define PRIi64 "li"
#define PRIdPTR "ld"
#define PRIxPTR "lx"
#define SCNd64 "ld"
#endif
