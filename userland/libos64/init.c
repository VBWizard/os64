// init.c — libos64's own startup, run by `launch` before main().
//
// See os64/runtime.h for why this exists as a named seam instead of a
// collection of lazy first-call checks scattered through the library.

#include "os64/runtime.h"
#include "os64/mem.h"      // os64_heap_init

void os64_runtime_init(const os64_env_block_t *env)
{
	// The environment first: it is pure data the kernel already mapped, and
	// publishing it before anything else means every later init step (and any
	// constructor that arrives in future) can read configuration out of it.
	os64_env_publish(env);

	// The heap goes up first and eagerly. Its init is cheap (zero a struct,
	// one syscall to register the report) and it buys an honest
	// /proc/<pid>/heap for every program from its first instruction —
	// including the ones that never call malloc at all.
	os64_heap_init();
}
