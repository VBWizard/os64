#include "test_framework.h"
#include "panic.h"

#include "memory/kmalloc.h"
#include "memory/memset.h"
#include "strcmp.h"
#include "strlen.h"
#include "memory/memcpy.h"
#include "memory/vma.h"
#include "memory/arena.h"
#include "memory/task_arena.h"
#include "allocator.h"
#include "paging.h"
#include "exceptions.h"
#include "dlist.h"
#include <stdint.h>
#include "smp_core.h"
#include "task.h"
#include "scheduler.h"
#include "time.h"
#include "driver/filesystem/vfs/vfs.h"
#include "shared_object.h"
#include "env.h"

extern volatile uint64_t kPageFaultCount;
extern task_t *kKernelTask;
extern vfs_filesystem_t *kRootFilesystem;

static test_case_t g_test_cases[TEST_MAX_CASES];
static size_t g_test_case_count = 0;
static bool g_framework_initialized = false;

bool test_vma_file_backed_page_fault_resolved(void);
bool test_vma_partial_page_bss_zero_filled(void);

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

// Test that a write to a CoW-mapped page:
//   (a) triggers exactly one page fault,
//   (b) the write succeeds (page is remapped writable),
//   (c) the rest of the page is a faithful copy of the original,
//   (d) the original physical page is not modified,
//   (e) the physical page behind the VMA changes (private copy allocated).
static bool test_vma_cow_write(void)
{
    task_t *task = get_core_local_storage()->task;
    uintptr_t test_addr = 0x61000000;  // well clear of other test VAs

    // Allocate a physical source page and fill it with a known pattern.
    uintptr_t orig_phys = allocate_memory_aligned(PAGE_SIZE);
    if (!orig_phys)
        TEST_FAIL("cow: failed to allocate source page");
    memset((void *)(orig_phys | kHHDMOffset), 0xAB, PAGE_SIZE);

    // Create a CoW VMA (PROT_WRITE so the page fault handler grants write access
    // once the page is privatised) and map the source page read-only.  The absence
    // of PAGE_WRITE on the mapping is what causes the CoW fault on the first write.
    vma_t *vma = vma_create(test_addr, test_addr + PAGE_SIZE,
                            PROT_READ | PROT_WRITE, MAP_PRIVATE, NULL, 0);
    if (!vma)
        TEST_FAIL("cow: failed to create VMA");
    vma->cow = true;
    vma_add(task, vma);
    paging_map_page((pt_entry_t *)task->pml4v, test_addr, orig_phys,
                    PAGE_PRESENT | PAGE_USER);  // intentionally no PAGE_WRITE

    uint64_t faults_before = kPageFaultCount;

    // Write to the CoW page.  This must fault, copy the page, remap writable,
    // and allow the CPU to retry the store — all transparently.
    volatile uint8_t *ptr = (volatile uint8_t *)test_addr;
    *ptr = 0xCD;

    // Exactly one CoW fault must have fired.
    if (kPageFaultCount != faults_before + 1)
        TEST_FAIL("cow: expected exactly 1 page fault");

    // The write must have landed in the private copy.
    if (*ptr != 0xCD)
        TEST_FAIL("cow: write did not persist after CoW fault");

    // The rest of the page must carry the original fill (content was copied).
    if (*(volatile uint8_t *)(test_addr + 1) != 0xAB)
        TEST_FAIL("cow: CoW page content was not copied from the original");

    // The original physical page must be intact.
    if (((volatile uint8_t *)(orig_phys | kHHDMOffset))[0] != 0xAB)
        TEST_FAIL("cow: original physical page was modified by CoW");

    // The physical page now backing the VMA must be a different page.
    uintptr_t new_phys = paging_walk_paging_table((pt_entry_t *)task->pml4v, test_addr);
    if (new_phys == orig_phys)
        TEST_FAIL("cow: physical page was not replaced with a private copy");

    // Cleanup: unmap, remove VMA, free both physical pages.
    paging_unmap_page((pt_entry_t *)task->pml4v, test_addr);
    if (task->mmaps != NULL && vma->listItem != NULL)
        dlist_remove(task->mmaps, vma->listItem);
    vma_destroy(vma);
    free_memory(orig_phys);
    kfree((void *)(new_phys | kHHDMOffset)); // new_phys was kmalloc_aligned in the CoW handler

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

    // The test ELF spans two pages: _start on page 1 (0x400000) and page2_func
    // on page 2 (0x401000).  At least two demand-page faults must have fired —
    // one per page.  This catches any regression of the vma->loaded-per-VMA bug
    // where the second fault in the same VMA would panic instead of mapping.
    if (kPageFaultCount < faults_before + 2) {
        printd(DEBUG_TESTS, "\tFAIL: test_elf_loader - expected >=2 page faults, got %lu\n",
               kPageFaultCount - faults_before);
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

// Success sentinel arg_echo.c returns after verifying argc/argv/env.  A failure
// instead returns 0xE00000xx identifying exactly which invariant broke — see
// kernel/test/elf/arg_echo.c.
#define ARG_ECHO_RETVAL 0x00A11600DUL

// Regression test for argument/environment passing.  Guards two fixes made
// together: task_setup_entry() must latch RDI/RSI/RDX AFTER argc/argv/env are
// populated (not before, when they are still zero), and task_create()'s argv
// construction must copy the strings into the task's own blob with
// TASK_ARGV_VIRT-relative pointers (rather than dangling into caller memory).
static bool test_task_args(void)
{
    if (kRootFilesystem == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_task_args (no root filesystem mounted)\n");
        return true;
    }

    // Launch /bin/arg_echo with a known argv.  The fixture asserts argc==3,
    // argv==TASK_ARGV_VIRT, argv[0..2] == {"/bin/arg_echo","hello","world"},
    // argv[3]==NULL, and a non-empty env at TASK_ENV_VIRT.
    char *args[] = { "/bin/arg_echo", "hello", "world" };
    task_t *task = task_create("/bin/arg_echo", 3, args, kKernelTask, true, THREAD_NO_AFFINITY);
    if (task == NULL) {
        printd(DEBUG_TESTS, "\tFAIL: test_task_args - task_create returned NULL\n");
        return false;
    }

    // Seed one environment variable so the fixture can confirm env *content*
    // (not just the pointer) flowed through.  env is already mapped at
    // TASK_ENV_VIRT, so writing the shared page is visible to the task.
    env_set(task->env, "OSTEST", "1");

    scheduler_submit_new_task(task);

    // Poll until the task exits or we time out (~1 second at 100 ticks/sec).
    for (int i = 0; i < 100 && !task->exited; i++)
        wait(10);

    if (!task->exited) {
        printd(DEBUG_TESTS, "\tFAIL: test_task_args - task did not exit within 1 second\n");
        return false;
    }

    if (task->retVal != ARG_ECHO_RETVAL) {
        printd(DEBUG_TESTS, "\tFAIL: test_task_args - retVal=0x%lx, expected 0x%lx "
               "(0xE00000xx identifies the failed check)\n",
               task->retVal, ARG_ECHO_RETVAL);
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_task_args (argc/argv/env delivered correctly)\n");
    return true;
}

// dyn_consumer.c calls shlib_add(2,3) twice and packs both results:
//   call 1: shlib_counter 42 -> 43 (this task's newly-privatized copy), returns 2+3+43=48
//   call 2: reads back 43 from that SAME private copy, becomes 44, returns 2+3+44=49
// Two different tasks should get the IDENTICAL packed value despite both
// writing to what started out as the same physical page — if CoW isolation
// were broken, the second task to run would see the first task's
// already-incremented counter instead. See kernel/test/elf/dyn_consumer.c
// and kernel/test/shlib/libtest.c.
#define DYN_CONSUMER_EXPECTED_PACKED 0x300031UL

static bool test_dynamic_linking(void)
{
    if (kRootFilesystem == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_dynamic_linking (no root filesystem mounted)\n");
        return true;
    }

    task_t *task_a = task_create("/bin/dyn_consumer", 0, NULL, kKernelTask, true, THREAD_NO_AFFINITY);
    if (task_a == NULL) {
        printd(DEBUG_TESTS, "\tFAIL: test_dynamic_linking - task_create (task A) returned NULL\n");
        return false;
    }
    scheduler_submit_new_task(task_a);

    task_t *task_b = task_create("/bin/dyn_consumer", 0, NULL, kKernelTask, true, THREAD_NO_AFFINITY);
    if (task_b == NULL) {
        printd(DEBUG_TESTS, "\tFAIL: test_dynamic_linking - task_create (task B) returned NULL\n");
        return false;
    }
    scheduler_submit_new_task(task_b);

    // Poll until both tasks exit or we time out (~1 second at 100 ticks/sec).
    for (int i = 0; i < 100 && (!task_a->exited || !task_b->exited); i++)
        wait(10);

    if (!task_a->exited || !task_b->exited) {
        printd(DEBUG_TESTS, "\tFAIL: test_dynamic_linking - task(s) did not exit within 1 second\n");
        return false;
    }

    if (task_a->retVal != DYN_CONSUMER_EXPECTED_PACKED) {
        printd(DEBUG_TESTS, "\tFAIL: test_dynamic_linking - task A retVal=0x%lx, expected 0x%lx\n",
               task_a->retVal, (uint64_t)DYN_CONSUMER_EXPECTED_PACKED);
        return false;
    }

    if (task_b->retVal != DYN_CONSUMER_EXPECTED_PACKED) {
        printd(DEBUG_TESTS, "\tFAIL: test_dynamic_linking - task B retVal=0x%lx, expected 0x%lx (CoW isolation broken?)\n",
               task_b->retVal, (uint64_t)DYN_CONSUMER_EXPECTED_PACKED);
        return false;
    }

    // Both tasks must have found the SAME shared_object_t via the registry
    // (cache-hit path, not a fresh load each time). shared_object_load_or_get
    // bumps refcount on every call — direct lookups AND the internal
    // recursive loads of DT_NEEDED dependencies. The main executable is
    // requested once per task_create plus once here: A + B + this call = 3.
    shared_object_t *exe_so = shared_object_load_or_get("/bin/dyn_consumer");
    if (exe_so == NULL) {
        printd(DEBUG_TESTS, "\tFAIL: test_dynamic_linking - dyn_consumer not found in registry after both tasks ran\n");
        return false;
    }
    if (exe_so->refcount != 3) {
        printd(DEBUG_TESTS, "\tFAIL: test_dynamic_linking - dyn_consumer refcount=%u, expected 3 (task A + task B + this lookup)\n",
               exe_so->refcount);
        return false;
    }

    // libtest.so, by contrast, is loaded as dyn_consumer's DT_NEEDED
    // dependency exactly ONCE — the second task_create cache-hits the
    // already-loaded executable and never re-walks its deps — so: that one
    // dependency edge + this lookup = 2. It must also be exactly the object
    // dyn_consumer's own dependency scope points at (per-object symbol
    // resolution — see shared_object.h's deps[]).
    shared_object_t *so = shared_object_load_or_get("/lib/libtest.so");
    if (so == NULL) {
        printd(DEBUG_TESTS, "\tFAIL: test_dynamic_linking - libtest.so not found in registry after both tasks ran\n");
        return false;
    }
    if (so->refcount != 2) {
        printd(DEBUG_TESTS, "\tFAIL: test_dynamic_linking - libtest.so refcount=%u, expected 2 (dyn_consumer's dep edge + this lookup)\n",
               so->refcount);
        return false;
    }
    if (exe_so->dep_count != 1 || exe_so->deps[0] != so) {
        printd(DEBUG_TESTS, "\tFAIL: test_dynamic_linking - dyn_consumer's dependency scope doesn't point at the registry's libtest.so\n");
        return false;
    }

    // Task A and task B must share the SAME physical page backing
    // libtest.so's code segment — proving true cross-task physical sharing,
    // not silently-duplicated per-task copies — even though their .data
    // pages have since diverged via CoW.
    uintptr_t code_phys_a = paging_walk_paging_table((pt_entry_t *)task_a->pml4v, so->load_bias);
    uintptr_t code_phys_b = paging_walk_paging_table((pt_entry_t *)task_b->pml4v, so->load_bias);
    if (code_phys_a == 0 || code_phys_a == 0xbadbadba || code_phys_a != code_phys_b) {
        printd(DEBUG_TESTS, "\tFAIL: test_dynamic_linking - libtest.so code page not physically shared (A=0x%lx, B=0x%lx)\n",
               code_phys_a, code_phys_b);
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_dynamic_linking (symbol resolution, relocation, CoW isolation, and physical sharing all correct)\n");
    return true;
}

// ── ring-3 / syscall tests ───────────────────────────────────────────────────
// These are the first tests to launch a task with isKernelTask=false — actual
// CPL 3 execution, crossing back and forth through syscall_Enter/sysretq.
// Everything before them ran ELF fixtures at ring 0.

// Success sentinel syscall_smoke.c exits with after yield + write + explicit
// exit all succeed.  Failures exit with 0xE51Cxxxx codes identifying the step.
#define SYSCALL_SMOKE_RETVAL 0x0005E00DUL

static bool test_ring3_syscall_smoke(void)
{
    if (kRootFilesystem == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_ring3_syscall_smoke (no root filesystem mounted)\n");
        return true;
    }

    task_t *task = task_create("/bin/syscall_smoke", 0, NULL, kKernelTask, false, THREAD_NO_AFFINITY);
    if (task == NULL) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_syscall_smoke - task_create returned NULL\n");
        return false;
    }

    scheduler_submit_new_task(task);

    // Poll until the task exits or we time out (~1 second at 100 ticks/sec).
    for (int i = 0; i < 100 && !task->exited; i++)
        wait(10);

    if (!task->exited) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_syscall_smoke - task did not exit within 1 second "
               "(likely died crossing ring 3 <-> ring 0; check STAR/GDT/sysret)\n");
        return false;
    }

    if (task->retVal != SYSCALL_SMOKE_RETVAL) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_syscall_smoke - retVal=0x%lx, expected 0x%lx "
               "(0xE51Cxxxx identifies the failed step)\n",
               task->retVal, (uint64_t)SYSCALL_SMOKE_RETVAL);
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_ring3_syscall_smoke (ring3 launch, yield, write, explicit exit)\n");
    return true;
}

// exit_by_return.c just `return`s this from _start; it can only reach retVal
// via the seeded user-stack return address -> ring-3 exit trampoline ->
// SYSCALL_EXIT chain, so this asserts that whole path.
#define EXIT_BY_RETURN_MAGIC 0x2E7BEA57UL

static bool test_ring3_exit_by_return(void)
{
    if (kRootFilesystem == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_ring3_exit_by_return (no root filesystem mounted)\n");
        return true;
    }

    task_t *task = task_create("/bin/exit_by_return", 0, NULL, kKernelTask, false, THREAD_NO_AFFINITY);
    if (task == NULL) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_exit_by_return - task_create returned NULL\n");
        return false;
    }

    scheduler_submit_new_task(task);

    for (int i = 0; i < 100 && !task->exited; i++)
        wait(10);

    if (!task->exited) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_exit_by_return - task did not exit within 1 second "
               "(trampoline seed or mapping broken? see task_setup_ring3_exit_path)\n");
        return false;
    }

    if (task->retVal != EXIT_BY_RETURN_MAGIC) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_exit_by_return - retVal=0x%lx, expected 0x%lx\n",
               task->retVal, (uint64_t)EXIT_BY_RETURN_MAGIC);
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_ring3_exit_by_return (ret -> trampoline -> exit syscall)\n");
    return true;
}
// file_io.c walks the whole file-handle lifecycle at CPL 3: open /bin/hello,
// read+verify the ELF magic, seek (SET and END), open a bogus path (must fail
// in-band), close, double-close (must fail).  0xF11Exxxx identifies the step.
#define FILE_IO_RETVAL 0x0F11E60DUL

static bool test_ring3_file_io(void)
{
    if (kRootFilesystem == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_ring3_file_io (no root filesystem mounted)\n");
        return true;
    }

    task_t *task = task_create("/bin/file_io", 0, NULL, kKernelTask, false, THREAD_NO_AFFINITY);
    if (task == NULL) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_file_io - task_create returned NULL\n");
        return false;
    }

    scheduler_submit_new_task(task);

    // File I/O is real disk I/O through call_in_kernel_context — allow 2s.
    for (int i = 0; i < 200 && !task->exited; i++)
        wait(10);

    if (!task->exited) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_file_io - task did not exit within 2 seconds "
               "(open/read/seek wedged? check the call_in_kernel_context paths)\n");
        return false;
    }

    if (task->retVal != FILE_IO_RETVAL) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_file_io - retVal=0x%lx, expected 0x%lx "
               "(0xF11Exxxx identifies the failed step; see test/elf/file_io.c)\n",
               task->retVal, (uint64_t)FILE_IO_RETVAL);
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_ring3_file_io (open, read, seek SET/END, bogus open, close, double-close)\n");
    return true;
}

// (A dedicated /bin/hello test lived here briefly during userland bring-up;
// removed as redundant — ring3_syscall_smoke and ring3_exit_by_return already
// cover load-run-exit at CPL 3, and the HELLO boot-flow launch exercises the
// real app path. /bin/hello stays on the image for that launch.)

// ── env tests ────────────────────────────────────────────────────────────────

static bool test_env_create_empty(void)
{
    envpage_t *env = env_create();
    if (!env)
        TEST_FAIL("env_create returned NULL");
    if (env->count != 0)
        TEST_FAIL("fresh env has non-zero count");
    if (env->data_end != 0)
        TEST_FAIL("fresh env has non-zero data_end");
    if (env_get(env, "ANYTHING") != NULL)
        TEST_FAIL("env_get on empty env should return NULL");
    kfree(env);
    return true;
}

static bool test_env_set_and_get(void)
{
    envpage_t *env = env_create();
    if (!env) TEST_FAIL("env_create returned NULL");

    if (!env_set(env, "PATH", "/bin:/usr/bin"))
        TEST_FAIL("env_set returned false");
    if (env->count != 1)
        TEST_FAIL("count should be 1 after one set");

    const char *v = env_get(env, "PATH");
    if (!v)
        TEST_FAIL("env_get returned NULL for existing key");
    if (strcmp(v, "/bin:/usr/bin") != 0)
        TEST_FAIL("env_get returned wrong value");

    kfree(env);
    return true;
}

static bool test_env_update_shorter_val(void)
{
    envpage_t *env = env_create();
    if (!env) TEST_FAIL("env_create returned NULL");

    env_set(env, "HOME", "/home/longusername");
    env_set(env, "HOME", "/root");          // shorter replacement

    if (env->count != 1)
        TEST_FAIL("count should still be 1 after update");
    const char *v = env_get(env, "HOME");
    if (!v || strcmp(v, "/root") != 0)
        TEST_FAIL("env_get returned wrong value after shorter update");

    kfree(env);
    return true;
}

static bool test_env_update_longer_val(void)
{
    envpage_t *env = env_create();
    if (!env) TEST_FAIL("env_create returned NULL");

    env_set(env, "TERM", "vt100");
    env_set(env, "TERM", "xterm-256color");  // longer replacement

    if (env->count != 1)
        TEST_FAIL("count should still be 1 after update");
    const char *v = env_get(env, "TERM");
    if (!v || strcmp(v, "xterm-256color") != 0)
        TEST_FAIL("env_get returned wrong value after longer update");

    kfree(env);
    return true;
}

static bool test_env_multi_key(void)
{
    envpage_t *env = env_create();
    if (!env) TEST_FAIL("env_create returned NULL");

    env_set(env, "A", "alpha");
    env_set(env, "B", "bravo");
    env_set(env, "C", "charlie");

    if (env->count != 3)
        TEST_FAIL("count should be 3");
    if (strcmp(env_get(env, "A"), "alpha")   != 0) TEST_FAIL("wrong value for A");
    if (strcmp(env_get(env, "B"), "bravo")   != 0) TEST_FAIL("wrong value for B");
    if (strcmp(env_get(env, "C"), "charlie") != 0) TEST_FAIL("wrong value for C");
    if (env_get(env, "D") != NULL)
        TEST_FAIL("missing key should return NULL");

    kfree(env);
    return true;
}

static bool test_env_inherit_copies(void)
{
    envpage_t *parent = env_create();
    if (!parent) TEST_FAIL("env_create returned NULL");
    env_set(parent, "PATH", "/bin");
    env_set(parent, "USER", "root");

    envpage_t *child = env_inherit(parent);
    if (!child) TEST_FAIL("env_inherit returned NULL");

    if (strcmp(env_get(child, "PATH"), "/bin") != 0)
        TEST_FAIL("child missing PATH from parent");
    if (strcmp(env_get(child, "USER"), "root") != 0)
        TEST_FAIL("child missing USER from parent");

    kfree(parent);
    kfree(child);
    return true;
}

static bool test_env_inherit_independence(void)
{
    envpage_t *parent = env_create();
    if (!parent) TEST_FAIL("env_create returned NULL");
    env_set(parent, "SHARED", "original");

    envpage_t *child = env_inherit(parent);
    if (!child) TEST_FAIL("env_inherit returned NULL");

    // Child modifies its copy; parent must be unaffected.
    env_set(child, "SHARED", "modified");
    env_set(child, "CHILD_ONLY", "yes");

    if (strcmp(env_get(parent, "SHARED"), "original") != 0)
        TEST_FAIL("parent SHARED was modified by child");
    if (env_get(parent, "CHILD_ONLY") != NULL)
        TEST_FAIL("parent sees child-only key after inherit");

    kfree(parent);
    kfree(child);
    return true;
}

static bool test_env_count_accurate(void)
{
    envpage_t *env = env_create();
    if (!env) TEST_FAIL("env_create returned NULL");

    env_set(env, "X", "1");
    env_set(env, "Y", "2");
    if (env->count != 2) TEST_FAIL("count wrong after 2 sets");

    env_set(env, "X", "updated");   // update, not add
    if (env->count != 2) TEST_FAIL("count should stay 2 after update");

    env_set(env, "Z", "3");
    if (env->count != 3) TEST_FAIL("count wrong after add following update");

    kfree(env);
    return true;
}

static bool test_env_capacity_full(void)
{
    envpage_t *env = env_create();
    if (!env) TEST_FAIL("env_create returned NULL");

    // Fill the page with entries "K000\0V000\0", "K001\0V001\0", ...
    // Each entry is 5+5 = 10 bytes; capacity ~4084 bytes → ~408 entries max.
    bool got_false = false;
    char key[8], val[8];
    for (int i = 0; i < 600; i++) {
        // Build "K%03d" and "V%03d" by hand (no sprintf available here easily).
        key[0]='K'; key[1]='0'+(i/100)%10; key[2]='0'+(i/10)%10; key[3]='0'+i%10; key[4]='\0';
        val[0]='V'; val[1]='0'+(i/100)%10; val[2]='0'+(i/10)%10; val[3]='0'+i%10; val[4]='\0';
        if (!env_set(env, key, val)) {
            got_false = true;
            break;
        }
    }
    if (!got_false)
        TEST_FAIL("env_set never returned false — capacity check may be broken");

    // Entries set before the page filled must still be readable.
    if (env_get(env, "K000") == NULL)
        TEST_FAIL("K000 missing after fill");

    kfree(env);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────

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
    test_register("vma_cow_write", test_vma_cow_write, TEST_PHASE_PREBOOT);
    test_register("env_create_empty",        test_env_create_empty,          TEST_PHASE_PREBOOT);
    test_register("env_set_and_get",         test_env_set_and_get,           TEST_PHASE_PREBOOT);
    test_register("env_update_shorter_val",  test_env_update_shorter_val,    TEST_PHASE_PREBOOT);
    test_register("env_update_longer_val",   test_env_update_longer_val,     TEST_PHASE_PREBOOT);
    test_register("env_multi_key",           test_env_multi_key,             TEST_PHASE_PREBOOT);
    test_register("env_inherit_copies",      test_env_inherit_copies,        TEST_PHASE_PREBOOT);
    test_register("env_inherit_independence",test_env_inherit_independence,  TEST_PHASE_PREBOOT);
    test_register("env_count_accurate",      test_env_count_accurate,        TEST_PHASE_PREBOOT);
    test_register("env_capacity_full",       test_env_capacity_full,         TEST_PHASE_PREBOOT);
    test_register("task_arena_create_and_destroy", test_task_arena_create_and_destroy, TEST_PHASE_PREBOOT);
    test_register("task_arena_alloc_and_convert", test_task_arena_alloc_and_convert, TEST_PHASE_PREBOOT);
    test_register("task_arena_aligned_alloc", test_task_arena_aligned_alloc, TEST_PHASE_PREBOOT);
    test_register("task_arena_exhaustion", test_task_arena_exhaustion, TEST_PHASE_PREBOOT);
    test_register("vma_file_backed_page_fault_resolved", test_vma_file_backed_page_fault_resolved, TEST_PHASE_POSTBOOT);
    test_register("vma_partial_page_bss_zero_filled", test_vma_partial_page_bss_zero_filled, TEST_PHASE_POSTBOOT);
    test_register("elf_loader", test_elf_loader, TEST_PHASE_POSTBOOT);
    test_register("task_args", test_task_args, TEST_PHASE_POSTBOOT);
    test_register("dynamic_linking", test_dynamic_linking, TEST_PHASE_POSTBOOT);
    test_register("ring3_syscall_smoke", test_ring3_syscall_smoke, TEST_PHASE_POSTBOOT);
    test_register("ring3_exit_by_return", test_ring3_exit_by_return, TEST_PHASE_POSTBOOT);
    test_register("ring3_file_io", test_ring3_file_io, TEST_PHASE_POSTBOOT);
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
        // panic(), not a bare cli/hlt: panic force-drains the log buffer to
        // serial before halting.  The old halt stranded this message — and any
        // test results logd hadn't drained yet — in the ring buffer forever.
        panic("Test framework: %u %s test(s) failed. System halted.\n",
              (unsigned int)failed, label);
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
