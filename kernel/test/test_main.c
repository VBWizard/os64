#include "test_framework.h"

#include "memory/kmalloc.h"
#include "memory/memset.h"
#include "memory/vma.h"
#include "exceptions.h"
#include "dlist.h"
#include <stdint.h>
#include "smp_core.h"

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

static bool test_dlist_basic_operations(void)
{
	dlist_t list;
	dlist_init(&list);

	if (list.head != NULL || list.tail != NULL || list.size != 0)
	{
		TEST_FAIL("dlist_init leaves list in non-empty state");
	}

	int value1 = 1;
	int value2 = 2;
	int value3 = 3;

	dlist_node_t* node1 = dlist_add(&list, &value1);
	if (list.size != 1 || list.head != node1 || list.tail != node1)
	{
		TEST_FAIL("dlist_add failed to set head/tail for first element");
	}

	dlist_node_t* node2 = dlist_add(&list, &value2);
	if (list.size != 2 || list.head != node1 || list.tail != node2)
	{
		TEST_FAIL("dlist_add failed to append second element");
	}
	if (node1->next != node2 || node2->prev != node1 || node2->next != NULL)
	{
		TEST_FAIL("dlist_add did not link nodes correctly");
	}

	dlist_node_t* node3 = dlist_add(&list, &value3);
	if (list.size != 3 || list.tail != node3)
	{
		TEST_FAIL("dlist_add failed to append third element");
	}
	if (node2->next != node3 || node3->prev != node2)
	{
		TEST_FAIL("dlist_add corrupts middle linkage");
	}

	dlist_remove(&list, node2);
	if (list.size != 2 || list.head != node1 || list.tail != node3)
	{
		TEST_FAIL("dlist_remove on middle node produced incorrect list metadata");
	}
	if (node1->next != node3 || node3->prev != node1)
	{
		TEST_FAIL("dlist_remove on middle node broke adjacency");
	}

	dlist_remove(&list, node1);
	if (list.size != 1 || list.head != node3 || list.tail != node3)
	{
		TEST_FAIL("dlist_remove on head did not promote next node");
	}
	if (node3->prev != NULL || node3->next != NULL)
	{
		TEST_FAIL("dlist_remove on head left stray links");
	}

	dlist_remove(&list, node3);
	if (list.size != 0 || list.head != NULL || list.tail != NULL)
	{
		TEST_FAIL("dlist_remove on final node did not empty list");
	}

	dlist_add(&list, &value1);
	dlist_add(&list, &value2);
	dlist_destroy(&list);
	if (list.size != 0 || list.head != NULL || list.tail != NULL)
	{
		TEST_FAIL("dlist_destroy failed to reset list state");
	}

	return true;
}

static bool test_vma_insert_and_lookup(void)
{
	task_t* task = kmalloc(sizeof(task_t));
	if (!task) {
		TEST_FAIL("test_vma: failed to allocate task");
	}

	memset(task, 0, sizeof(task_t));

	task->mmaps = kmalloc(sizeof(dlist_t));
	if (!task->mmaps) {
		TEST_FAIL("test_vma: failed to allocate mmaps list");
	}
	dlist_init(task->mmaps);

	uintptr_t start = 0x100000;
	uintptr_t end = 0x102000;
	vma_t* vma = vma_create(start, end, PROT_READ | PROT_WRITE, MAP_PRIVATE, NULL, 0);
	if (!vma) {
		TEST_FAIL("test_vma: failed to allocate vma");
	}

	vma_add(task, vma);

	vma_t* found = vma_lookup(task, 0x101000);
	if (!found || found != vma) {
		TEST_FAIL("vma_lookup failed to find inserted VMA");
	}

    found = vma_lookup(task, 0x11101000);
    if (found)
        TEST_FAIL("vma_lookup found inserted VMA when it should not have");

    dlist_destroy(task->mmaps);
	kfree(task->mmaps);
	vma_destroy(vma);
	kfree(task);

	return true;
}

bool test_vma_page_fault_resolved()
{
    uintptr_t test_addr = 0x400000; // Some unused safe test address
    vma_t *vma = vma_create(test_addr, test_addr + PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE, NULL, 0);
    vma_add(get_core_local_storage()->task, vma);

    uint64_t old_faults = kPageFaultCount;

    volatile uint32_t *ptr = (volatile uint32_t *)test_addr;
    *ptr = 0xBEEFCAFE; // Should trigger page fault and be resolved

    return (kPageFaultCount == old_faults + 1);
}

static void register_builtin_tests(void)
{
    test_register("kmalloc_not_null", test_kmalloc_not_null);
    test_register("page_fault_test_mode_returns", test_page_fault_does_not_panic_when_testing_flag_is_set);
    test_register("dlist_basic_operations", test_dlist_basic_operations);
    test_register("vma_insert_and_lookup", test_vma_insert_and_lookup);
    test_register("vma_page_fault_resolved", test_vma_page_fault_resolved);
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
