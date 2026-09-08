#include <stddef.h>

// This has ordinary memcmp semantics. Upstream's constant-time comparison
// primitives remain in upstream and do not pass through this function.
int os64_bearssl_memcmp(const void *a, const void *b, size_t size)
{
    const unsigned char *left = a, *right = b;
    for (size_t i = 0; i < size; i++) {
        if (left[i] != right[i])
            return left[i] < right[i] ? -1 : 1;
    }
    return 0;
}
