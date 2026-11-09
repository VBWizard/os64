// Exercise the production optional CLS lookup without reading a host GS base.
#include "smp_core.h"

extern int puts(const char *);
extern _Noreturn void exit(int);

volatile core_local_storage_t kCoreLocalStorage[MAX_CPUS];
static uint64_t test_gs_base;

static void check(bool condition, const char *message)
{
    if (!condition) {
        puts(message);
        exit(1);
    }
}

uint64_t rdmsr64(unsigned index)
{
    check(index == IA32_GS_BASE, "FAIL: wrong MSR");
    return test_gs_base;
}

int main(void)
{
    uintptr_t first = (uintptr_t)&kCoreLocalStorage[0];
    uintptr_t limit = (uintptr_t)&kCoreLocalStorage[MAX_CPUS];
    uintptr_t invalid[] = {0, 1, 4095, first - 1, first + 1,
                           first + sizeof(core_local_storage_t) - 1,
                           limit, UINT64_MAX};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        test_gs_base = invalid[i];
        check(try_get_core_local_storage() == NULL,
              "FAIL: accepted an invalid or unaligned GS base");
    }
    for (unsigned i = 0; i < MAX_CPUS; i++) {
        core_local_storage_t *cls = (core_local_storage_t *)&kCoreLocalStorage[i];
        test_gs_base = (uintptr_t)cls;
        check(try_get_core_local_storage() == NULL,
              "FAIL: accepted a slot before its self pointer was initialized");
        cls->self = (void *)first;
        if (i != 0)
            check(try_get_core_local_storage() == NULL,
                  "FAIL: accepted another slot's self pointer");
        cls->self = cls;
        check(try_get_core_local_storage() == cls,
              "FAIL: refused initialized CLS");
    }
    // A different core having initialized its slot cannot make GS zero ready.
    test_gs_base = 0;
    check(try_get_core_local_storage() == NULL,
          "FAIL: another core made an unset GS base ready");
    puts("PASS: optional CLS lookup");
    return 0;
}
