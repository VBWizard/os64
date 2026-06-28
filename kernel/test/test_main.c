#include "test_framework.h"

#include "memory/kmalloc.h"
#include "memory/memset.h"
#include "memory/vma.h"
#include "memory/arena.h"
#include "memory/task_arena.h"
#include "paging.h"
#include "exceptions.h"
#include "dlist.h"
#include <stdint.h>
#include "smp_core.h"
#include "task.h"
#include "scheduler.h"
#include "time.h"
#include "driver/filesystem/vfs/vfs.h"

extern volatile uint64_t kPageFaultCount;
extern task_t *kKernelTask;
extern vfs_filesystem_t *kRootFilesystem;

static test_case_t g_test_cases[TEST_MAX_CASES];
static size_t g_test_case_count = 0;
static bool g_framework_initialized = false;

bool test_vma_file_backed_page_fault_resolved(void);

bool test_register(const char *name, bool (*func)(void), int phase)
{
    if (g_test_case_count >= TEST_MAX_CASES) {
        const char *test_name = name ? name : "<unnamed>";
        printd(DEBUG_TESTS, "[Test] Failed to register %s (capacity reached)\n", test_name);
        return false;
    }

    g_test_cases[g_test_case_count].name = name;
    g_test_cases[g_test_case_count].func = func;
    g_test_cases[g_test_case_count].phase = phase;
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
    uintptr_t test_addr = 0x60000000; // Keep clear of ELF loader test mapping.
    task_t *task = get_core_local_storage()->task;
    vma_t *vma = vma_create(test_addr, test_addr + PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE, NULL, 0);
    vma_add(task, vma);

    uint64_t old_faults = kPageFaultCount;

    volatile uint32_t *ptr = (volatile uint32_t *)test_addr;
    *ptr = 0xBEEFCAFE; // Should trigger page fault and be resolved

    bool ok = (kPageFaultCount == old_faults + 1);

    paging_unmap_page((pt_entry_t *)task->pml4v, test_addr);
    if (task->mmaps != NULL && vma->listItem != NULL) {
        dlist_remove(task->mmaps, vma->listItem);
    }
    vma_destroy(vma);

    return ok;
}

static bool test_arena_create_and_destroy(void)
{
    arena_t *arena = arena_create(4096);
    if (arena == NULL) {
        TEST_FAIL("arena_create returned NULL");
    }

    if (arena->capacity != 4096) {
        TEST_FAIL("arena capacity incorrect");
    }

    if (arena->offset != 0) {
        TEST_FAIL("arena offset should be 0 after creation");
    }

    if (arena_remaining(arena) != 4096) {
        TEST_FAIL("arena_remaining incorrect after creation");
    }

    arena_destroy(arena);
    return true;
}

static bool test_arena_basic_alloc(void)
{
    arena_t *arena = arena_create(1024);
    if (arena == NULL) {
        TEST_FAIL("arena_create returned NULL");
    }

    // Allocate some memory
    void *ptr1 = arena_alloc(arena, 100);
    if (ptr1 == NULL) {
        TEST_FAIL("arena_alloc returned NULL for first allocation");
    }

    if (arena->offset != 100) {
        TEST_FAIL("arena offset incorrect after first allocation");
    }

    // Allocate more
    void *ptr2 = arena_alloc(arena, 200);
    if (ptr2 == NULL) {
        TEST_FAIL("arena_alloc returned NULL for second allocation");
    }

    if (arena->offset != 300) {
        TEST_FAIL("arena offset incorrect after second allocation");
    }

    // Verify pointers are sequential
    if ((uintptr_t)ptr2 != (uintptr_t)ptr1 + 100) {
        TEST_FAIL("arena allocations not sequential");
    }

    // Verify remaining space
    if (arena_remaining(arena) != 724) {
        TEST_FAIL("arena_remaining incorrect");
    }

    arena_destroy(arena);
    return true;
}

static bool test_arena_aligned_alloc(void)
{
    // Test 16-byte alignment
    arena_t *arena = arena_create(1024);
    if (arena == NULL) {
        TEST_FAIL("arena_create returned NULL");
    }

    // First allocate 1 byte to misalign
    void *ptr1 = arena_alloc(arena, 1);
    if (ptr1 == NULL) {
        TEST_FAIL("arena_alloc returned NULL");
    }

    // Now allocate with 16-byte alignment
    void *ptr2 = arena_alloc_aligned(arena, 64, 16);
    if (ptr2 == NULL) {
        TEST_FAIL("arena_alloc_aligned returned NULL");
    }

    // Verify 16-byte alignment
    if (((uintptr_t)ptr2 & 0xF) != 0) {
        TEST_FAIL("arena_alloc_aligned did not return 16-byte aligned pointer");
    }

    // Allocate 3 bytes to misalign again
    arena_alloc(arena, 3);

    // Now allocate with 64-byte alignment
    void *ptr3 = arena_alloc_aligned(arena, 32, 64);
    if (ptr3 == NULL) {
        TEST_FAIL("arena_alloc_aligned returned NULL for 64-byte alignment");
    }

    // Verify 64-byte alignment
    if (((uintptr_t)ptr3 & 0x3F) != 0) {
        TEST_FAIL("arena_alloc_aligned did not return 64-byte aligned pointer");
    }

    arena_destroy(arena);

    // Test page alignment with a larger arena to ensure we have space
    // Use 8KB arena for page-aligned test since we need padding + allocation
    arena_t *arena2 = arena_create(8192);
    if (arena2 == NULL) {
        TEST_FAIL("arena_create returned NULL for page-aligned test");
    }

    void *ptr4 = arena_alloc_aligned(arena2, 128, 4096);
    if (ptr4 == NULL) {
        TEST_FAIL("arena_alloc_aligned returned NULL for page-aligned allocation");
    }

    // Verify page alignment
    if (((uintptr_t)ptr4 & 0xFFF) != 0) {
        TEST_FAIL("arena_alloc_aligned did not return page-aligned pointer");
    }

    arena_destroy(arena2);
    return true;
}

static bool test_arena_reset(void)
{
    arena_t *arena = arena_create(1024);
    if (arena == NULL) {
        TEST_FAIL("arena_create returned NULL");
    }

    // Allocate some memory
    arena_alloc(arena, 500);
    if (arena->offset != 500) {
        TEST_FAIL("arena offset incorrect after allocation");
    }

    // Reset
    arena_reset(arena);
    if (arena->offset != 0) {
        TEST_FAIL("arena offset not reset to 0");
    }

    if (arena_remaining(arena) != 1024) {
        TEST_FAIL("arena_remaining incorrect after reset");
    }

    // Should be able to allocate again
    void *ptr = arena_alloc(arena, 100);
    if (ptr == NULL) {
        TEST_FAIL("arena_alloc failed after reset");
    }

    arena_destroy(arena);
    return true;
}

static bool test_arena_exhaustion(void)
{
    arena_t *arena = arena_create(100);
    if (arena == NULL) {
        TEST_FAIL("arena_create returned NULL");
    }

    // Allocate up to capacity
    void *ptr1 = arena_alloc(arena, 100);
    if (ptr1 == NULL) {
        TEST_FAIL("arena_alloc failed for exact capacity");
    }

    // Should fail now - arena exhausted
    void *ptr2 = arena_alloc(arena, 1);
    if (ptr2 != NULL) {
        TEST_FAIL("arena_alloc should return NULL when exhausted");
    }

    // Reset and try again
    arena_reset(arena);
    void *ptr3 = arena_alloc(arena, 50);
    if (ptr3 == NULL) {
        TEST_FAIL("arena_alloc failed after reset");
    }

    arena_destroy(arena);
    return true;
}

static bool test_task_arena_create_and_destroy(void)
{
    // Use the current kernel task for testing
    task_t *task = get_core_local_storage()->task;
    if (task == NULL) {
        TEST_FAIL("No current task available for testing");
    }

    // Save original next virtual address
    uintptr_t orig_next_virt = task->taskMemoryNextVirt;

    task_arena_t *arena = task_arena_create(task, 4096);
    if (arena == NULL) {
        TEST_FAIL("task_arena_create returned NULL");
    }

    if (arena->capacity != 4096) {
        TEST_FAIL("task_arena capacity incorrect");
    }

    if (arena->offset != 0) {
        TEST_FAIL("task_arena offset should be 0 after creation");
    }

    if (arena->owner != task) {
        TEST_FAIL("task_arena owner incorrect");
    }

    if (arena->task_buffer == NULL) {
        TEST_FAIL("task_arena task_buffer is NULL");
    }

    if (arena->kernel_buffer == NULL) {
        TEST_FAIL("task_arena kernel_buffer is NULL");
    }

    if (task_arena_remaining(arena) != 4096) {
        TEST_FAIL("task_arena_remaining incorrect after creation");
    }

    task_arena_destroy(arena);

    // Restore original next virtual address for cleanup
    task->taskMemoryNextVirt = orig_next_virt;

    return true;
}

static bool test_task_arena_alloc_and_convert(void)
{
    task_t *task = get_core_local_storage()->task;
    if (task == NULL) {
        TEST_FAIL("No current task available for testing");
    }

    uintptr_t orig_next_virt = task->taskMemoryNextVirt;

    task_arena_t *arena = task_arena_create(task, 4096);
    if (arena == NULL) {
        TEST_FAIL("task_arena_create returned NULL");
    }

    // Allocate some memory
    void *task_ptr = task_arena_alloc(arena, 64);
    if (task_ptr == NULL) {
        TEST_FAIL("task_arena_alloc returned NULL");
    }

    // Verify pointer is within task buffer range
    if ((uintptr_t)task_ptr < (uintptr_t)arena->task_buffer ||
        (uintptr_t)task_ptr >= (uintptr_t)arena->task_buffer + arena->capacity) {
        TEST_FAIL("task_arena_alloc returned pointer outside task buffer");
    }

    // Convert to kernel pointer
    void *kernel_ptr = task_arena_to_kernel_ptr(arena, task_ptr);
    if (kernel_ptr == NULL) {
        TEST_FAIL("task_arena_to_kernel_ptr returned NULL");
    }

    // Verify kernel pointer is within kernel buffer range
    if ((uintptr_t)kernel_ptr < (uintptr_t)arena->kernel_buffer ||
        (uintptr_t)kernel_ptr >= (uintptr_t)arena->kernel_buffer + arena->capacity) {
        TEST_FAIL("task_arena_to_kernel_ptr returned pointer outside kernel buffer");
    }

    // Convert back to task pointer
    void *task_ptr2 = task_arena_to_task_ptr(arena, kernel_ptr);
    if (task_ptr2 != task_ptr) {
        TEST_FAIL("task_arena_to_task_ptr did not return original pointer");
    }

    // Write via kernel pointer, verify offset relationship
    *(uint32_t *)kernel_ptr = 0xDEADBEEF;

    // Verify the offset matches (task_ptr and kernel_ptr should be at same offset)
    uintptr_t task_offset = (uintptr_t)task_ptr - (uintptr_t)arena->task_buffer;
    uintptr_t kernel_offset = (uintptr_t)kernel_ptr - (uintptr_t)arena->kernel_buffer;
    if (task_offset != kernel_offset) {
        TEST_FAIL("task and kernel pointer offsets don't match");
    }

    task_arena_destroy(arena);
    task->taskMemoryNextVirt = orig_next_virt;

    return true;
}

static bool test_task_arena_aligned_alloc(void)
{
    task_t *task = get_core_local_storage()->task;
    if (task == NULL) {
        TEST_FAIL("No current task available for testing");
    }

    uintptr_t orig_next_virt = task->taskMemoryNextVirt;

    task_arena_t *arena = task_arena_create(task, 8192);
    if (arena == NULL) {
        TEST_FAIL("task_arena_create returned NULL");
    }

    // Allocate 1 byte to misalign
    task_arena_alloc(arena, 1);

    // Allocate with 16-byte alignment
    void *ptr = task_arena_alloc_aligned(arena, 64, 16);
    if (ptr == NULL) {
        TEST_FAIL("task_arena_alloc_aligned returned NULL");
    }

    // Verify 16-byte alignment
    if (((uintptr_t)ptr & 0xF) != 0) {
        TEST_FAIL("task_arena_alloc_aligned did not return 16-byte aligned pointer");
    }

    // Allocate with 64-byte alignment
    void *ptr2 = task_arena_alloc_aligned(arena, 32, 64);
    if (ptr2 == NULL) {
        TEST_FAIL("task_arena_alloc_aligned returned NULL for 64-byte alignment");
    }

    // Verify 64-byte alignment
    if (((uintptr_t)ptr2 & 0x3F) != 0) {
        TEST_FAIL("task_arena_alloc_aligned did not return 64-byte aligned pointer");
    }

    task_arena_destroy(arena);
    task->taskMemoryNextVirt = orig_next_virt;

    return true;
}

static bool test_task_arena_exhaustion(void)
{
    task_t *task = get_core_local_storage()->task;
    if (task == NULL) {
        TEST_FAIL("No current task available for testing");
    }

    uintptr_t orig_next_virt = task->taskMemoryNextVirt;

    // Create a small arena (will be rounded to PAGE_SIZE)
    task_arena_t *arena = task_arena_create(task, 4096);
    if (arena == NULL) {
        TEST_FAIL("task_arena_create returned NULL");
    }

    // Allocate up to capacity
    void *ptr1 = task_arena_alloc(arena, 4096);
    if (ptr1 == NULL) {
        TEST_FAIL("task_arena_alloc failed for exact capacity");
    }

    // Should fail now - arena exhausted
    void *ptr2 = task_arena_alloc(arena, 1);
    if (ptr2 != NULL) {
        TEST_FAIL("task_arena_alloc should return NULL when exhausted");
    }

    // Reset and try again
    task_arena_reset(arena);
    void *ptr3 = task_arena_alloc(arena, 2048);
    if (ptr3 == NULL) {
        TEST_FAIL("task_arena_alloc failed after reset");
    }

    task_arena_destroy(arena);
    task->taskMemoryNextVirt = orig_next_virt;

    return true;
}

// Magic value serial_ping.S leaves in RAX before ret.
#define ELF_TEST_RETVAL 0xE1F0CA11UL

static bool test_elf_loader(void)
{
    if (kRootFilesystem == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_elf_loader (no root filesystem mounted)\n");
        return true;
    }

    uint64_t faults_before = kPageFaultCount;

    task_t *elf_task = task_create("/bin/test_elf", 0, NULL, kKernelTask, true, THREAD_NO_AFFINITY);
    if (elf_task == NULL) {
        printd(DEBUG_TESTS, "\tFAIL: test_elf_loader - task_create returned NULL\n");
        return false;
    }

    scheduler_submit_new_task(elf_task);

    // Poll until the task exits or we time out (~1 second at 100 ticks/sec).
    for (int i = 0; i < 100 && !elf_task->exited; i++)
        wait(10);

    if (!elf_task->exited) {
        printd(DEBUG_TESTS, "\tFAIL: test_elf_loader - task did not exit within 1 second\n");
        return false;
    }

    if (kPageFaultCount == faults_before) {
        printd(DEBUG_TESTS, "\tFAIL: test_elf_loader - no page faults (demand paging did not fire)\n");
        return false;
    }

    if (elf_task->retVal != ELF_TEST_RETVAL) {
        printd(DEBUG_TESTS, "\tFAIL: test_elf_loader - retVal=0x%lx, expected 0x%lx\n",
               elf_task->retVal, ELF_TEST_RETVAL);
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_elf_loader (demand-paged, retVal correct, exited cleanly)\n");
    return true;
}

static void register_builtin_tests(void)
{
    test_register("kmalloc_not_null", test_kmalloc_not_null, TEST_PHASE_PREBOOT);
    test_register("page_fault_test_mode_returns", test_page_fault_does_not_panic_when_testing_flag_is_set, TEST_PHASE_PREBOOT);
    test_register("dlist_basic_operations", test_dlist_basic_operations, TEST_PHASE_PREBOOT);
    test_register("arena_create_and_destroy", test_arena_create_and_destroy, TEST_PHASE_PREBOOT);
    test_register("arena_basic_alloc", test_arena_basic_alloc, TEST_PHASE_PREBOOT);
    test_register("arena_aligned_alloc", test_arena_aligned_alloc, TEST_PHASE_PREBOOT);
    test_register("arena_reset", test_arena_reset, TEST_PHASE_PREBOOT);
    test_register("arena_exhaustion", test_arena_exhaustion, TEST_PHASE_PREBOOT);
    test_register("vma_insert_and_lookup", test_vma_insert_and_lookup, TEST_PHASE_PREBOOT);
    test_register("vma_page_fault_resolved", test_vma_page_fault_resolved, TEST_PHASE_PREBOOT);
    test_register("task_arena_create_and_destroy", test_task_arena_create_and_destroy, TEST_PHASE_PREBOOT);
    test_register("task_arena_alloc_and_convert", test_task_arena_alloc_and_convert, TEST_PHASE_PREBOOT);
    test_register("task_arena_aligned_alloc", test_task_arena_aligned_alloc, TEST_PHASE_PREBOOT);
    test_register("task_arena_exhaustion", test_task_arena_exhaustion, TEST_PHASE_PREBOOT);
    test_register("vma_file_backed_page_fault_resolved", test_vma_file_backed_page_fault_resolved, TEST_PHASE_POSTBOOT);
    test_register("elf_loader", test_elf_loader, TEST_PHASE_POSTBOOT);
}

void test_framework_init(void)
{
    if (g_framework_initialized) {
        return;
    }

    g_framework_initialized = true;
    register_builtin_tests();
}

static void test_run_phase(int phase, const char *label)
{
    if (!g_framework_initialized) {
        test_framework_init();
    }

    size_t passed = 0;
    size_t failed = 0;

    printd(DEBUG_TESTS, "BUILT-IN TESTS: Running %s tests:\n", label);
    for (size_t index = 0; index < g_test_case_count; ++index)
    {
        test_case_t *test = &g_test_cases[index];
        const char *name = test->name ? test->name : "<unnamed>";

        if (test->phase != phase) {
            continue;
        }

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

void test_run_preboot(void)
{
    test_run_phase(TEST_PHASE_PREBOOT, "pre-boot");
}

void test_run_postboot(void)
{
    test_run_phase(TEST_PHASE_POSTBOOT, "post-boot");
}
