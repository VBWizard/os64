#ifndef OS64_LOCK_H
#define OS64_LOCK_H

#include <stdbool.h>
#include <stdint.h>
#include "os64/proc.h"

// Zero-initialize before sharing. Non-recursive; no signal-handler use.
// Prefer short sections: contenders spin briefly, then yield their CPU.
typedef struct { uint32_t value; } os64_lock_t;
#define OS64_LOCK_INIT {0}

static inline bool os64_lock_try(os64_lock_t *lock)
{
    return __atomic_exchange_n(&lock->value, 1u, __ATOMIC_ACQUIRE) == 0;
}

static inline void os64_lock_acquire(os64_lock_t *lock)
{
    while (!os64_lock_try(lock)) {
        for (unsigned i = 0; i < 64; ++i) {
            if (!__atomic_load_n(&lock->value, __ATOMIC_RELAXED)) break;
            __asm__ volatile("pause");
        }
        if (__atomic_load_n(&lock->value, __ATOMIC_RELAXED)) os64_yield();
    }
}

static inline void os64_lock_release(os64_lock_t *lock)
{
    __atomic_store_n(&lock->value, 0u, __ATOMIC_RELEASE);
}
#endif
