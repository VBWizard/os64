// env.c — libos64 environment access (os64_getenv, declared in os64/proc.h).
//
// __os64_env is stored by launch.S before main() runs: the kernel enters the
// program with RDX = the env block's task-local mapping (TASK_ENV_VIRT), and
// launch writes that register here so the library can answer os64_getenv()
// from anywhere without every program threading envp through its call tree.
//
// Pure computation over the read-only ABI block (abi/include/os64/env.h) —
// no syscalls, no allocation. Like fmt.c and args.c, that makes it
// host-testable with plain gcc; tools/test_fmt_host.c drives it.

#include "os64/env.h"
#include "os64/proc.h"

// Written exactly once, by launch.S, before main. NULL means the kernel
// passed no environment (possible for bare fixtures) — getenv then answers
// NULL for everything, which is the honest answer.
const os64_env_block_t *__os64_env;

static int e_streq(const char *a, const char *b)
{
	while (*a != '\0' && *a == *b)
	{
		a++;
		b++;
	}
	return *a == *b;
}

const char *os64_getenv(const char *key)
{
	const os64_env_block_t *env = __os64_env;
	if (env == NULL || key == NULL)
		return NULL;

	// Walk the packed key\0value\0 pairs — same walk the kernel's env_get
	// does, over the same bytes (the layout is the ABI; see os64/env.h).
	const char *ptr = env->data;
	const char *end = env->data + env->data_end;

	while (ptr < end)
	{
		const char *k = ptr;
		while (ptr < end && *ptr)
			ptr++;
		ptr++;                      // step over the key's NUL
		const char *v = ptr;
		while (ptr < end && *ptr)
			ptr++;
		ptr++;                      // step over the value's NUL

		if (e_streq(k, key))
			return v;
	}
	return NULL;
}
