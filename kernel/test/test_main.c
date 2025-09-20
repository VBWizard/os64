#include "test_framework.h"

#include "memory/kmalloc.h"

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

static void register_builtin_tests(void)
{
    test_register("kmalloc_not_null", test_kmalloc_not_null);
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

    for (size_t index = 0; index < g_test_case_count; ++index) {
        test_case_t *test = &g_test_cases[index];
        const char *name = test->name ? test->name : "<unnamed>";

        bool result = false;
        if (test->func != NULL) {
            result = test->func();
        } else {
            printd(DEBUG_TESTS, "    FAIL: %s\n", "Test function pointer is NULL");
        }

        if (result) {
            ++passed;
            printd(DEBUG_TESTS, "[Test] %s... OK\n", name);
        } else {
            ++failed;
            printd(DEBUG_TESTS, "[Test] %s... FAIL\n", name);
        }
    }

    printd(DEBUG_TESTS, "%u passed, %u failed\n", (unsigned int)passed, (unsigned int)failed);

    if (failed > 0) {
        printd(DEBUG_TESTS, "Test framework detected failures. System halted.\n");
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }
}
