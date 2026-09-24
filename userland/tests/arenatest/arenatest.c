#include "os64/os64.h"

#define ARENA_PASS 0xA2E7A000u

#define ARENA_CHECK(condition) do { \
    if (!(condition)) { \
        os64_printf("arenatest: FAIL line %d: %s\n", __LINE__, #condition); \
        os64_exit(1); \
    } \
} while (0)

#include "cases.h"

int main(void)
{
    ARENA_CHECK(os64_heap_verify() == 0);
    arena_test_cases();
    ARENA_CHECK(os64_heap_verify() == 0);
    os64_printf("arenatest: PASS (alignment, growth, limits, strings, zeroing, 50 frames, heap)\n");
    return (int)ARENA_PASS;
}
