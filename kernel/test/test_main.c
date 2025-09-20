#include "test_framework.h"

#include "memory/kmalloc.h"
#include "exceptions.h"
#include <stdint.h>

static test_case_t g_test_cases[TEST_MAX_CASES];
static size_t g_test_case_count = 0;
static bool g_framework_initialized = false;

bool test_register(const char *name, bool (*func)(void))
{
    if (g_test_case_count >= TEST_MAX_CASES) {
        const char *test_name = name ? name : "<unnamed>";
        printd(DEBUG_TESTS, "[Test] Failed to register %s (capacity reached)\n", test_name);
        return false;
    }

    g_test_cases[g_test_case_count].name = name;
    g_test_cases[g_test_case_count].func = func;
    g_test_case_count++;
    return true;
}

static bool test_kmalloc_not_null(void)
{
    void *ptr = kmalloc(64);
    if (ptr == NULL) {
        TEST_FAIL("kmalloc returned NULL");
    }

    kfree(ptr);
    return true;
}

static bool test_page_fault_does_not_panic_when_testing_flag_is_set(void)
{
	void *resume = &&after_fault;
	kTestingPageFaults = true;
	kTestingPageFaultResumeRip = (uint64_t)(uintptr_t)resume;
	*((volatile uint32_t *)0x0) = 0x1234;
after_fault:
	kTestingPageFaults = false;
	kTestingPageFaultResumeRip = 0;
	return true;
}

static void register_builtin_tests(void)
{
    test_register("kmalloc_not_null", test_kmalloc_not_null);
    test_register("page_fault_test_mode_returns", test_page_fault_does_not_panic_when_testing_flag_is_set);
}

void test_framework_init(void)
{
    if (g_framework_initialized) {
        return;
    }

    g_framework_initialized = true;
    register_builtin_tests();
}

void test_run_all(void)
{
    if (!g_framework_initialized) {
        test_framework_init();
    }

    size_t passed = 0;
    size_t failed = 0;

    printd(DEBUG_TESTS, "BUILT-IN TESTS: Running all tests:\n");
    for (size_t index = 0; index < g_test_case_count; ++index)
    {
        test_case_t *test = &g_test_cases[index];
        const char *name = test->name ? test->name : "<unnamed>";

        bool result = false;
        if (test->func != NULL) {
            result = test->func();
        } else {
            printd(DEBUG_TESTS, "\tFAIL: %s\n", "Invalid test. Test function pointer is NULL");
        }

        if (result) {
            ++passed;
            printd(DEBUG_TESTS, "\t[Test] %s... OK\n", name);
        } else {
            ++failed;
            printd(DEBUG_TESTS, "\t[Test] %s... FAIL\n", name);
        }
    }

    printd(DEBUG_TESTS, "BUILT-IN TESTS: %u passed, %u failed\n", (unsigned int)passed, (unsigned int)failed);

    if (failed > 0) {
        printd(DEBUG_TESTS, "Test framework detected failures. System halted.\n");
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }
}
