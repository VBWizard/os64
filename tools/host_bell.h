// host_bell.h — os64/bell.h for the host harnesses (tools/host_bell.c).
//
// libfetch's pool parks a fetch on a bell (pool.h). On the host a bell is a
// mutex, a condition and the same three rules: a ring before the wait is
// kept, two rings are one, one waiter at a time. The harness also gets what
// only a stand-in can give: a count of the bells alive, a wait that never
// blocks (for a harness with one thread, where nobody could ring), a way to
// interrupt a waiter as a caught signal would, and a refusal to make one.
//
// A RING OF A BELL THAT IS NOT ONE ABORTS THE PROGRAM. On os64 that ring is
// a write to whatever handle took the number; here it is the failure of the
// case that made it.
#ifndef HOST_BELL_H
#define HOST_BELL_H
#include <stdbool.h>
#include <stdint.h>
#include "os64/bell.h"

extern bool host_bell_never_blocks;     // an unrung wait answers OS64_ERR_TIMEOUT at once,
extern void (*host_bell_slept)(uint64_t ms);    // and tells this, when set, how long it would have slept
extern bool host_bell_refuse_make;      // os64_bell_make fails
unsigned host_bell_alive(void);         // bells made and not closed
unsigned long host_bell_waits(void);    // waits that found the bell unrung and parked
void host_bell_interrupt(os64_bell_t *bell);   // a parked waiter answers OS64_INTERRUPTED
#endif
