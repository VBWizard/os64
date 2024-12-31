#ifndef PRINTD_H
#define PRINTD_H

#include "CONFIG.h"
extern __uint128_t kDebugLevel;

void printd(__uint128_t debug_level, const char *fmt, ...);

#endif