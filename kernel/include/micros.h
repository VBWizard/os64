#ifndef MICROS_H
#define MICROS_H

#include <stdint.h>

// Microseconds since boot, never running backward, from any core and any
// context (micros.c). The value ring 3's micros() returns.
uint64_t micros_now(void);

#endif // MICROS_H
