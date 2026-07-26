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
#include "os64/str.h"

// Written exactly once, by launch.S, before main. NULL means the kernel
// passed no environment (possible for bare fixtures) — getenv then answers
// NULL for everything, which is the honest answer.
const os64_env_block_t *__os64_env;

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

		if (os64_streq(k, key))
			return v;
	}
	return NULL;
}

/// @brief Increments the passed os64_envent_t.index and returns the
/// environment block entry at that index. One-based index, so pass 0 for the first index
/// NOTE: Unconditionally increments os64_envent_t.index, even if that makes it point past 
/// the end of the environment for the next call
/// @param buffer Pointer to an existing os64_envent_t. Filled by os64_env_next
/// @return For success: 0, for failure a positive error code
int32_t os64_env_next(os64_envent_t *buffer)
{
    const os64_env_block_t *env = __os64_env;
    uint32_t userIndex = 0;
    //currIndex starts at 1 because the index to find (userIndex) can never be 0
    uint32_t currIndex = 1;
    //retVal starts at 1 (error: index too large) and gets set to 0 if all goes well and it isn't
    int32_t retVal = 1;
    if (env == NULL)
        return 2; //No environment block

    if (!buffer)
        return 3; //Bad buffer (null pointer)
    else if (buffer->index + 1 == 0)
        return 4; //Buffer overflow inevitible
    else
        userIndex = buffer->index + 1;

    const char *ptr = env->data;
    const char *end = env->data + env->data_end;

    //Iterate the environment block which is filled with NULL padded
    //key value pairs. (e.g. key\0value\0key\0value\0)
    //Stop and return the k/v at the user specified index +1
    while (ptr < end)
    {
        const char *k = ptr;
        while (ptr < end && *ptr)
            ptr++;
        ptr++; // step over the key's NUL
        const char *v = ptr;
        while (ptr < end && *ptr)
            ptr++;
        ptr++; // step over the value's NUL
        if (userIndex == currIndex)
        {
            os64_strcopy((char*)buffer->key, OS64_ENV_STR_MAX + 1, k);
            os64_strcopy((char*)buffer->value, OS64_ENV_STR_MAX + 1, v);
            retVal = 0; //Index found, no error
            break;
        }
        currIndex++;
    }
    if (!retVal) //success
        buffer->index = currIndex;

    return retVal;
}
