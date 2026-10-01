#include "os64/os64.h"
#include "os64/mem.h"
#include "cases.h"

int main(void)
{
    int failures = js_support_string_cases();
    if (os64_malloc_size(NULL) != 0) failures++;
    const size_t requests[] = {0, 1, 300, 5000, 128 * 1024};
    for (size_t i = 0; i < sizeof(requests) / sizeof(requests[0]); i++) {
        unsigned char *p = os64_malloc(requests[i]);
        if (!p) { failures++; continue; }
        size_t capacity = os64_malloc_size(p);
        if (capacity < requests[i] || !capacity) failures++;
        else {
            p[capacity - 1] = 0xA5;
            if (os64_realloc(p, SIZE_MAX) != NULL ||
                os64_malloc_size(p) != capacity || p[capacity - 1] != 0xA5)
                failures++;
        }
        os64_free(p);
    }
    os64_printf("jssupporttest: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
