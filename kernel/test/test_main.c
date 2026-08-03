#include "test_framework.h"
#include "panic.h"
#include "BasicRenderer.h"   // printf — failing test NAMES go on the glass (P5 has no serial)

#include "memory/kmalloc.h"
#include "memory/memset.h"
#include "strcmp.h"
#include "strlen.h"
#include "memory/memcpy.h"
#include "memory/memcmp.h"
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
#include "kernel.h"   // kTicksSinceStart — the TCP tests time their failures
#include "driver/filesystem/vfs/vfs.h"
#include "driver/filesystem/ext2/ext2_vfs.h"   // ext2_fops/ext2_dops (real-partition test)
#include "ff.h"   // FR_OK/FR_EXIST — the dops->mkdir seam leaks FatFs codes
                  // today (known wart); test_vfs_write_mkdir names them
#include "shared_object.h"
#include "env.h"
#include "sprintf.h"
#include "driver/net/net_device.h"   // test_net_wire — the driver's first packets
#include "driver/net/net_wire.h"     // Phase 2 stack tests build real wire bytes
#include "driver/net/net_checksum.h"
#include "driver/net/ethernet.h"
#include "driver/net/arp.h"
#include "driver/net/ipv4.h"
#include "driver/net/icmp.h"
#include "driver/net/udp.h"
#include "driver/net/dhcp.h"
#include "driver/net/udp_conn.h"
#include "driver/net/tcp.h"
#include "driver/net/icmp_conn.h"
#include "os64/net.h"                // OS64_NET_ERR_* — refusals carry reasons (abi)

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

// Two file_io fixtures at ONCE: the FatFs reentrancy regression. Before the
// ff_mutex hooks (FF_FS_REENTRANT), two cores inside f_open/f_read on the same
// volume shared the FATFS sector-window buffer unsynchronized — open() from
// ring 3 made that trivially reachable. Both tasks open the same file, read,
// seek, and close concurrently; both must come back bit-perfect.
static bool test_ring3_file_io_concurrent(void)
{
    if (kRootFilesystem == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_ring3_file_io_concurrent (no root filesystem mounted)\n");
        return true;
    }

    task_t *a = task_create("/bin/file_io", 0, NULL, kKernelTask, false, THREAD_NO_AFFINITY);
    task_t *b = task_create("/bin/file_io", 0, NULL, kKernelTask, false, THREAD_NO_AFFINITY);
    if (a == NULL || b == NULL) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_file_io_concurrent - task_create returned NULL\n");
        return false;
    }

    // Submit back-to-back so their lifetimes overlap as much as the scheduler
    // allows (with 8 cores they genuinely run simultaneously).
    scheduler_submit_new_task(a);
    scheduler_submit_new_task(b);

    for (int i = 0; i < 300 && !(a->exited && b->exited); i++)
        wait(10);

    if (!a->exited || !b->exited) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_file_io_concurrent - task(s) did not exit within 3 seconds "
               "(a=%d b=%d; deadlock in the FAT volume lock?)\n", a->exited, b->exited);
        return false;
    }

    if (a->retVal != FILE_IO_RETVAL || b->retVal != FILE_IO_RETVAL) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_file_io_concurrent - retVals 0x%lx / 0x%lx, expected 0x%lx "
               "(one task saw corrupted data => reentrancy regression)\n",
               a->retVal, b->retVal, (uint64_t)FILE_IO_RETVAL);
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_ring3_file_io_concurrent (two tasks, same volume+file, simultaneous)\n");
    return true;
}

// redirect_io.c proves file redirection through spawn: a child's stdout/stdin
// pointed at files, the parent closing its handle copies while the child still
// writes (the handleRefCount test — before it, that close freed the FIL under
// the child), and the full `upper < in > out` shape.  0x2ED1xxxx names the step.
#define REDIRECT_IO_RETVAL 0x2ED1600DUL

static bool test_ring3_redirect_io(void)
{
    // The fixture CREATES files on the root filesystem — on a read-only root
    // (ext2) there is nothing to create them on. Skipping is the honest
    // verdict: the write path isn't broken, it's absent by design. The FAT
    // boot entry runs this for real.
    if (kRootFilesystem != NULL && kRootFilesystem->fops->write == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_ring3_redirect_io (root filesystem is read-only)\n");
        return true;
    }
    if (kRootFilesystem == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_ring3_redirect_io (no root filesystem mounted)\n");
        return true;
    }

    task_t *task = task_create("/bin/redirect_io", 0, NULL, kKernelTask, false, THREAD_NO_AFFINITY);
    if (task == NULL) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_redirect_io - task_create returned NULL\n");
        return false;
    }

    scheduler_submit_new_task(task);

    // The fixture spawns and reaps two children of its own — allow 3s.
    for (int i = 0; i < 300 && !task->exited; i++)
        wait(10);

    if (!task->exited) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_redirect_io - task did not exit within 3 seconds "
               "(child wedged on a dead file handle? refcount regression?)\n");
        return false;
    }

    if (task->retVal != REDIRECT_IO_RETVAL) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_redirect_io - retVal=0x%lx, expected 0x%lx "
               "(0x2ED1xxxx identifies the failed step; see test/elf/redirect_io.c)\n",
               task->retVal, (uint64_t)REDIRECT_IO_RETVAL);
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_ring3_redirect_io (child stdout->file, early parent close, upper < in > out)\n");
    return true;
}

// dir_list.c walks /bin and / through open(path,"d") + readdir at CPL 3:
// entry names/sizes/DIR flags, sticky end-of-directory, type safety (readdir
// on a file handle refuses), bogus paths, close semantics. 0x0D12xxxx codes.
#define DIR_LIST_RETVAL 0x0D12600DUL

static bool test_ring3_dir_list(void)
{
    if (kRootFilesystem == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_ring3_dir_list (no root filesystem mounted)\n");
        return true;
    }

    task_t *task = task_create("/bin/dir_list", 0, NULL, kKernelTask, false, THREAD_NO_AFFINITY);
    if (task == NULL) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_dir_list - task_create returned NULL\n");
        return false;
    }

    scheduler_submit_new_task(task);

    for (int i = 0; i < 200 && !task->exited; i++)
        wait(10);

    if (!task->exited) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_dir_list - task did not exit within 2 seconds\n");
        return false;
    }

    if (task->retVal != DIR_LIST_RETVAL) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_dir_list - retVal=0x%lx, expected 0x%lx "
               "(0x0D12xxxx identifies the failed step; see test/elf/dir_list.c)\n",
               task->retVal, (uint64_t)DIR_LIST_RETVAL);
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_ring3_dir_list (readdir /bin + /, flags, sticky EOF, type safety)\n");
    return true;
}

// ── The ext2 real-partition test ─────────────────────────────────────────────
// Partition 2 of the disk image is formatted by the HOST's mkfs.ext2 and
// populated by debugfs (see the GNUmakefile disk rule): os64 never wrote a
// byte of it. This test mounts it with the ext2 driver and reads it back —
// the honest proof that we parse real ext2, not our own private dialect.
//
// pattern.bin's 16-byte records are self-describing ("00001234:os64e2\n" =
// record 1234 at byte 1234*16), so seeking into the direct, single-indirect,
// and double-indirect regions and asking the record its own name catches any
// off-by-one the block-map walk could commit. (Region math is documented in
// tools/gen_ext2_testdata.py; block size is pinned to 1024 at mkfs time.)
#define EXT2_PATTERN_RECORDS 98304UL
#define EXT2_PATTERN_SIZE    (EXT2_PATTERN_RECORDS * 16)

static bool ext2_check_record(vfs_filesystem_t *fs, vfs_file_t *f, uint64_t offset, const char *region)
{
    char got[17], want[20];

    if (fs->fops->seek(f, (long)offset, SEEK_SET) < 0) {
        printd(DEBUG_TESTS, "\tFAIL: ext2 - seek to %lu (%s region) failed\n", offset, region);
        return false;
    }
    if (fs->fops->read(f, got, 16) != 16) {
        printd(DEBUG_TESTS, "\tFAIL: ext2 - read at %lu (%s region) short\n", offset, region);
        return false;
    }
    got[16] = '\0';
    sprintf(want, "%08lu:os64e2\n", (unsigned long)(offset / 16));
    if (strncmp(got, want, 16) != 0) {
        printd(DEBUG_TESTS, "\tFAIL: ext2 - %s region record at %lu: got '%s', want '%s'\n",
               region, offset, got, want);
        return false;
    }
    return true;
}

static bool test_ext2_real_partition(void)
{
    // Find the ext2 partition by detected filesystem type — the boot flow's
    // superblock probe (filesystem.c) marks it during storage init.
    vfs_filesystem_t *fs = NULL;
    for (int d = 0; d < kBlockDeviceInfoCount && fs == NULL; d++)
    {
        block_device_info_t *dev = &kBlockDeviceInfo[d];
        if (dev->block_device == NULL || dev->block_device->partition_table == NULL)
            continue;
        for (int p = 0; p < dev->block_device->partition_table->partCount; p++)
        {
            if (dev->block_device->partition_table->parts[p]->filesystemType != FILESYSTEM_TYPE_EXT2)
                continue;
            // Assemble a minimal filesystem object by hand, deliberately
            // BYPASSING the mount table: this is the driver-level test, and
            // driving the driver directly keeps it meaningful even now that
            // the namespace also mounts this partition (test_mount_table
            // covers the routed path). Read-only driver, no locks — a second
            // ext2_fs_t on the same partition is harmless.
            fs = kmalloc(sizeof(vfs_filesystem_t));
            if (fs == NULL)
                return false;
            memset(fs, 0, sizeof(vfs_filesystem_t));
            fs->partNumber = p;
            fs->block_device_info = dev;
            fs->bops = dev->block_device->ops;
            fs->fops = &ext2_fops;
            fs->dops = &ext2_dops;
            break;
        }
    }
    if (fs == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_ext2_real_partition (no ext2 partition detected)\n");
        return true;
    }

    if (ext2_initialize_filesystem(fs) != 0) {
        printd(DEBUG_TESTS, "\tFAIL: test_ext2_real_partition - superblock/groups mount failed\n");
        return false;
    }

    // 1. A small file, byte-for-byte: content authored by Linux.
    static const char hello_expect[] =
        "Hello from a real ext2 filesystem — written by Linux, read by os64!\n";
    vfs_file_t *f = NULL;
    char buf[128];
    if (fs->fops->open(&f, "/hello.txt", "r", fs) != 0) {
        printd(DEBUG_TESTS, "\tFAIL: test_ext2_real_partition - open /hello.txt failed\n");
        return false;
    }
    int n = fs->fops->read(f, buf, sizeof(buf));
    fs->fops->close(f);
    if (n != (int)(sizeof(hello_expect) - 1) || strncmp(buf, hello_expect, sizeof(hello_expect) - 1) != 0) {
        printd(DEBUG_TESTS, "\tFAIL: test_ext2_real_partition - /hello.txt content mismatch (n=%d)\n", n);
        return false;
    }

    // 2. Path resolution three directories deep.
    if (fs->fops->open(&f, "/dir1/dir2/deep.txt", "r", fs) != 0) {
        printd(DEBUG_TESTS, "\tFAIL: test_ext2_real_partition - open /dir1/dir2/deep.txt failed\n");
        return false;
    }
    n = fs->fops->read(f, buf, sizeof(buf));
    fs->fops->close(f);
    if (n <= 0 || strncmp(buf, "the deep file", 13) != 0) {
        printd(DEBUG_TESTS, "\tFAIL: test_ext2_real_partition - deep.txt content mismatch\n");
        return false;
    }

    // 3. The block-map workout: size via tell-at-end, then self-describing
    //    records from each mapping regime.
    if (fs->fops->open(&f, "/pattern.bin", "r", fs) != 0) {
        printd(DEBUG_TESTS, "\tFAIL: test_ext2_real_partition - open /pattern.bin failed\n");
        return false;
    }
    fs->fops->seek(f, 0, SEEK_END);
    if ((uint64_t)fs->fops->tell(f) != EXT2_PATTERN_SIZE) {
        printd(DEBUG_TESTS, "\tFAIL: test_ext2_real_partition - pattern.bin size %d, want %lu\n",
               fs->fops->tell(f), EXT2_PATTERN_SIZE);
        fs->fops->close(f);
        return false;
    }
    bool ok = ext2_check_record(fs, f, 0,                      "first")
           && ext2_check_record(fs, f, 4096,                   "direct")
           && ext2_check_record(fs, f, 100000,                 "single-indirect")
           && ext2_check_record(fs, f, 1000000,                "double-indirect")
           && ext2_check_record(fs, f, EXT2_PATTERN_SIZE - 16, "last");
    fs->fops->close(f);
    if (!ok)
        return false;

    // 4. The root listing through the fs-neutral dirent seam — same contract
    //    ls uses, different filesystem, zero caller changes. (lost+found is
    //    mkfs's own droppings and proves the listing is real.)
    vfs_directory_t *dir = NULL;
    if (fs->dops->open(&dir, "/", fs) != 0) {
        printd(DEBUG_TESTS, "\tFAIL: test_ext2_real_partition - opendir / failed\n");
        return false;
    }
    os64_dirent_t de;
    bool saw_hello = false, saw_pattern = false, saw_dir1 = false, saw_lf = false;
    int r;
    while ((r = fs->dops->read(dir, &de)) == 1)
    {
        if (strncmp(de.name, "hello.txt", 10) == 0 && !(de.flags & OS64_DE_DIR))
            saw_hello = true;
        if (strncmp(de.name, "pattern.bin", 12) == 0 && de.size == EXT2_PATTERN_SIZE)
            saw_pattern = true;
        if (strncmp(de.name, "dir1", 5) == 0 && (de.flags & OS64_DE_DIR))
            saw_dir1 = true;
        if (strncmp(de.name, "lost+found", 11) == 0 && (de.flags & OS64_DE_DIR))
            saw_lf = true;
    }
    fs->dops->close(dir);
    if (r != 0 || !saw_hello || !saw_pattern || !saw_dir1 || !saw_lf) {
        printd(DEBUG_TESTS, "\tFAIL: test_ext2_real_partition - root listing (r=%d hello=%d pattern=%d dir1=%d lost+found=%d)\n",
               r, saw_hello, saw_pattern, saw_dir1, saw_lf);
        return false;
    }

    // 5. Absence must fail in-band, like everything else in this kernel.
    if (fs->fops->open(&f, "/no/such/file", "r", fs) == 0) {
        printd(DEBUG_TESTS, "\tFAIL: test_ext2_real_partition - bogus path opened\n");
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_ext2_real_partition (mkfs.ext2-authored: hello, deep path, "
           "direct/single/double-indirect records, root listing, bogus path)\n");
    return true;
}

// The mount table: longest-prefix routing over the LIVE table built at boot.
// Semantics first (boundaries, tails), then real I/O through whatever
// secondary mounts this boot actually produced — "/ext2" when FAT is root,
// "/fat" when ext2 is (both partitions carry known content, so either way
// there is something to verify end to end).
static bool test_mount_table(void)
{
    if (kMountCount < 1 || kRootFilesystem == NULL) {
        printd(DEBUG_TESTS, "\tFAIL: test_mount_table - no mounts (count=%d)\n", kMountCount);
        return false;
    }

    // 1. The root always resolves, tail unchanged.
    const char *tail = NULL;
    vfs_filesystem_t *fs = vfs_resolve_mount("/", &tail);
    if (fs != kRootFilesystem || strncmp(tail, "/", 2) != 0) {
        printd(DEBUG_TESTS, "\tFAIL: test_mount_table - '/' did not resolve to root\n");
        return false;
    }

    // 2. Every non-root entry: exact prefix → its fs with tail "/", a child
    //    path → tail with the prefix stripped, and a NON-boundary lookalike
    //    ("/fatzz") must fall through to the root, never to the mount.
    char probe[VFS_MOUNT_PREFIX_MAX + 8];
    for (int i = 0; i < kMountCount; i++)
    {
        vfs_mount_entry_t *m = &kMountTable[i];
        if (m->prefix_len == 1)
            continue;

        fs = vfs_resolve_mount(m->prefix, &tail);
        if (fs != m->fs || strncmp(tail, "/", 2) != 0) {
            printd(DEBUG_TESTS, "\tFAIL: test_mount_table - '%s' exact match wrong (tail=%s)\n",
                   m->prefix, tail ? tail : "NULL");
            return false;
        }

        sprintf(probe, "%s/x", m->prefix);
        fs = vfs_resolve_mount(probe, &tail);
        if (fs != m->fs || strncmp(tail, "/x", 3) != 0) {
            printd(DEBUG_TESTS, "\tFAIL: test_mount_table - '%s' child tail wrong (tail=%s)\n",
                   probe, tail ? tail : "NULL");
            return false;
        }

        sprintf(probe, "%szz", m->prefix);
        fs = vfs_resolve_mount(probe, &tail);
        if (fs == m->fs) {
            printd(DEBUG_TESTS, "\tFAIL: test_mount_table - '%s' matched prefix '%s' (boundary leak)\n",
                   probe, m->prefix);
            return false;
        }
    }

    // 3. Routed I/O through each secondary mount: list its root via dops
    //    (must yield at least one entry), plus a content check where we know
    //    the content: /ext2/hello.txt is Linux-authored, /fat/partition_info
    //    ships on every FAT image.
    int verified = 0;
    for (int i = 0; i < kMountCount; i++)
    {
        vfs_mount_entry_t *m = &kMountTable[i];
        if (m->prefix_len == 1)
            continue;

        vfs_directory_t *dir = NULL;
        fs = vfs_resolve_mount(m->prefix, &tail);
        if (fs->dops == NULL || fs->dops->open == NULL ||
            fs->dops->open(&dir, tail, fs) != 0) {
            printd(DEBUG_TESTS, "\tFAIL: test_mount_table - opendir %s failed\n", m->prefix);
            return false;
        }
        os64_dirent_t de;
        int entries = 0;
        while (fs->dops->read(dir, &de) == 1)
            entries++;
        fs->dops->close(dir);
        if (entries == 0) {
            printd(DEBUG_TESTS, "\tFAIL: test_mount_table - %s listed empty\n", m->prefix);
            return false;
        }

        const char *file_probe = NULL;
        const char *expect = NULL;
        if (strncmp(m->prefix, "/ext2", 6) == 0) {
            file_probe = "/ext2/hello.txt";
            expect = "Hello from a real ext2";
        } else if (strncmp(m->prefix, "/fat", 5) == 0) {
            file_probe = "/fat/partition_info";
            expect = NULL;   // content varies; opening + reading >0 is the check
        }
        if (file_probe != NULL)
        {
            vfs_file_t *f = NULL;
            char buf[64];
            fs = vfs_resolve_mount(file_probe, &tail);
            if (fs != m->fs ||
                fs->fops->open(&f, tail, "r", fs) != 0) {
                printd(DEBUG_TESTS, "\tFAIL: test_mount_table - open %s failed\n", file_probe);
                return false;
            }
            int n = fs->fops->read(f, buf, sizeof(buf));
            fs->fops->close(f);
            if (n <= 0 || (expect != NULL && strncmp(buf, expect, strlen(expect)) != 0)) {
                printd(DEBUG_TESTS, "\tFAIL: test_mount_table - %s content wrong (n=%d)\n",
                       file_probe, n);
                return false;
            }
            verified++;
        }
    }

    printd(DEBUG_TESTS, "\tPASS: test_mount_table (%d mounts, %d routed content checks)\n",
           kMountCount, verified);
    return true;
}

// map_unmap.c drives the heap primitive at CPL 3: anonymous regions demand-
// paged and zeroed, guard-page separation, region independence, whole-region
// unmap with strict base validation. 0x3A9xxxxx names the failed step.
#define MAP_UNMAP_RETVAL 0x03A9600DUL

static bool test_ring3_map_unmap(void)
{
    if (kRootFilesystem == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_ring3_map_unmap (no root filesystem mounted)\n");
        return true;
    }

    task_t *task = task_create("/bin/map_unmap", 0, NULL, kKernelTask, false, THREAD_NO_AFFINITY);
    if (task == NULL) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_map_unmap - task_create returned NULL\n");
        return false;
    }

    scheduler_submit_new_task(task);

    for (int i = 0; i < 200 && !task->exited; i++)
        wait(10);

    if (!task->exited) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_map_unmap - task did not exit within 2 seconds "
               "(fault storm in the demand pager? unmap freed a live page?)\n");
        return false;
    }

    if (task->retVal != MAP_UNMAP_RETVAL) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_map_unmap - retVal=0x%lx, expected 0x%lx "
               "(0x3A9xxxxx identifies the failed step; see test/elf/map_unmap.c)\n",
               task->retVal, (uint64_t)MAP_UNMAP_RETVAL);
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_ring3_map_unmap (zeroed demand pages, guard gap, independence, strict unmap)\n");
    return true;
}

// cwd_test.c proves kernel-owned "here" at CPL 3: getcwd/chdir, canonical
// ".." collapse, relative open AND relative spawn resolving against cwd, a
// failed chdir moving nothing, and inheritance via a self-spawned child that
// verifies where it was born. 0x0C3Dxxxx names the failed step.
#define CWD_TEST_RETVAL 0x0C3D600DUL

static bool test_ring3_cwd(void)
{
    if (kRootFilesystem == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_ring3_cwd (no root filesystem mounted)\n");
        return true;
    }

    task_t *task = task_create("/bin/cwd_test", 0, NULL, kKernelTask, false, THREAD_NO_AFFINITY);
    if (task == NULL) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_cwd - task_create returned NULL\n");
        return false;
    }

    scheduler_submit_new_task(task);

    // Spawns and reaps a child of its own — allow 3s.
    for (int i = 0; i < 300 && !task->exited; i++)
        wait(10);

    if (!task->exited) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_cwd - task did not exit within 3 seconds\n");
        return false;
    }

    if (task->retVal != CWD_TEST_RETVAL) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_cwd - retVal=0x%lx, expected 0x%lx "
               "(0x0C3Dxxxx identifies the failed step; see test/elf/cwd_test.c)\n",
               task->retVal, (uint64_t)CWD_TEST_RETVAL);
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_ring3_cwd (getcwd/chdir, canonicalization, relative open+spawn, inheritance)\n");
    return true;
}

// The root-filesystem WRITE path, properly inside the framework at last:
// create/write/read-back a file, mkdir, create/write/read-back inside the new
// directory. This is the useful half of the old boot-time testVFS() — which
// lived OUTSIDE the framework in tests.c, ran from kernel.c after the suite,
// and PANICKED on failure (its panic ate a Friday: "Root filesystem disk test
// failed: 4294967291" was this code trusting a then-void nvme write). Ported
// at Chris's call, 2026-07-19; the legacy block and tests.c are gone. The
// read half wasn't ported — file_io/dir_list/mount_table already cover reads
// through the real syscall path.
static bool test_vfs_write_mkdir(void)
{
    if (kRootFilesystem == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_vfs_write_mkdir (no root filesystem mounted)\n");
        return true;
    }
    // Writing needs a filesystem that writes AND a device that writes —
    // ext2 is read-only by design, so this SKIPs on the ext2-root boots and
    // runs for real on the FAT entry.
    if (kRootFilesystem->fops->write == NULL || kRootFilesystem->bops->write == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_vfs_write_mkdir (root filesystem is read-only)\n");
        return true;
    }

    static const char msg1[] = "Hello world from Chris!\n";        // heritage strings —
    static const char msg2[] = "Hello world from Chris too!\n";    // testVFS's originals
    char buf[64];
    vfs_file_t *f = NULL;

    // 1. Create, write, read back at the root.
    if (kRootFilesystem->fops->open(&f, "/test2", "c", kRootFilesystem) != 0) {
        printd(DEBUG_TESTS, "\tFAIL: test_vfs_write_mkdir - create /test2 failed\n");
        return false;
    }
    kRootFilesystem->fops->write(f, msg1, sizeof(msg1) - 1);
    kRootFilesystem->fops->close(f);
    f = NULL;
    if (kRootFilesystem->fops->open(&f, "/test2", "r", kRootFilesystem) != 0) {
        printd(DEBUG_TESTS, "\tFAIL: test_vfs_write_mkdir - reopen /test2 failed\n");
        return false;
    }
    int n = kRootFilesystem->fops->read(f, buf, sizeof(buf));
    kRootFilesystem->fops->close(f);
    if (n != (int)(sizeof(msg1) - 1) || strncmp(buf, msg1, sizeof(msg1) - 1) != 0) {
        printd(DEBUG_TESTS, "\tFAIL: test_vfs_write_mkdir - /test2 read-back mismatch (n=%d)\n", n);
        return false;
    }

    // 2. mkdir. The dops->mkdir seam leaks raw FatFs codes today (a known
    //    wart — no second filesystem implements mkdir yet to force the
    //    neutral contract): FR_OK fresh, FR_EXIST on a persistent image
    //    that's been through this test before. Both are success here.
    int r = kRootFilesystem->dops->mkdir("/testdir", kRootFilesystem);
    if (r != FR_OK && r != FR_EXIST) {
        printd(DEBUG_TESTS, "\tFAIL: test_vfs_write_mkdir - mkdir /testdir failed (%d)\n", r);
        return false;
    }

    // 3. A file INSIDE the new directory — proves the directory is real.
    if (kRootFilesystem->fops->open(&f, "/testdir/testfile", "c", kRootFilesystem) != 0) {
        printd(DEBUG_TESTS, "\tFAIL: test_vfs_write_mkdir - create /testdir/testfile failed\n");
        return false;
    }
    kRootFilesystem->fops->write(f, msg2, sizeof(msg2) - 1);
    kRootFilesystem->fops->close(f);
    f = NULL;
    if (kRootFilesystem->fops->open(&f, "/testdir/testfile", "r", kRootFilesystem) != 0) {
        printd(DEBUG_TESTS, "\tFAIL: test_vfs_write_mkdir - reopen /testdir/testfile failed\n");
        return false;
    }
    n = kRootFilesystem->fops->read(f, buf, sizeof(buf));
    kRootFilesystem->fops->close(f);
    if (n != (int)(sizeof(msg2) - 1) || strncmp(buf, msg2, sizeof(msg2) - 1) != 0) {
        printd(DEBUG_TESTS, "\tFAIL: test_vfs_write_mkdir - /testdir/testfile read-back mismatch (n=%d)\n", n);
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_vfs_write_mkdir (create/write/read-back, mkdir, nested file)\n");
    return true;
}

// stat_test.c proves the stat syscall at CPL 3: file with size, directory,
// the synthesized root entry, in-band absence, relative resolution, and
// routing across the mount table. 0x57A7xxxx names the failed step.
#define STAT_TEST_RETVAL 0x57A7600DUL

static bool test_ring3_stat(void)
{
    if (kRootFilesystem == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_ring3_stat (no root filesystem mounted)\n");
        return true;
    }

    task_t *task = task_create("/bin/stat_test", 0, NULL, kKernelTask, false, THREAD_NO_AFFINITY);
    if (task == NULL) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_stat - task_create returned NULL\n");
        return false;
    }

    scheduler_submit_new_task(task);

    // Six stats, all disk I/O through call_in_kernel_context — allow 2s.
    for (int i = 0; i < 200 && !task->exited; i++)
        wait(10);

    if (!task->exited) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_stat - task did not exit within 2 seconds\n");
        return false;
    }

    if (task->retVal != STAT_TEST_RETVAL) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_stat - retVal=0x%lx, expected 0x%lx "
               "(0x57A7xxxx identifies the failed step; see test/elf/stat_test.c)\n",
               task->retVal, (uint64_t)STAT_TEST_RETVAL);
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_ring3_stat (file, dir, root, absence, relative, cross-mount)\n");
    return true;
}

// sleep_test.c drives sleep(ms) + ticks(out) at CPL 3: the 2012 SIGSLEEP
// machinery's first ring-3 customer. It measures its own nap with the ticks
// syscall at the reported rate (so the assertion survives any future
// TICKS_PER_SECOND), proves sleep(0) is a yield and not a nap, and checks
// the stopwatch only runs forward. 0x51EExxxx names the failed step.
// Ctrl+C-interrupts-the-nap is deliberately NOT here — it's interactive,
// verified by hand like the rest of the SIGINT family.
#define SLEEP_TEST_RETVAL 0x51EE600DUL

static bool test_ring3_sleep(void)
{
    if (kRootFilesystem == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_ring3_sleep (no root filesystem mounted)\n");
        return true;
    }

    task_t *task = task_create("/bin/sleep_test", 0, NULL, kKernelTask, false, THREAD_NO_AFFINITY);
    if (task == NULL) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_sleep - task_create returned NULL\n");
        return false;
    }

    scheduler_submit_new_task(task);

    // The fixture sleeps 200ms on purpose; give it 3s so a loaded suite
    // never flakes the timeout while a genuine never-wakes bug still fails.
    for (int i = 0; i < 300 && !task->exited; i++)
        wait(10);

    if (!task->exited) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_sleep - task did not exit within 3 seconds "
               "(a sleeper processSignals never woke? deadline math off by an epoch?)\n");
        return false;
    }

    if (task->retVal != SLEEP_TEST_RETVAL) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_sleep - retVal=0x%lx, expected 0x%lx "
               "(0x51EExxxx identifies the failed step; see test/elf/sleep_test.c)\n",
               task->retVal, (uint64_t)SLEEP_TEST_RETVAL);
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_ring3_sleep (parked >= request at reported rate, sleep(0) yields, monotonic)\n");
    return true;
}

// Runs /bin/memory_test: the memory() syscall picture is sane AND the books
// balance — free + used == usable, exactly, at rest, mid-allocation, and
// after unmap (the fixture maps/touches/unmaps 256KB to watch the needle
// move). A failure here with FAIL_BOOKS_* means the allocator's extent
// ledger dropped or double-counted something: a real accounting bug, not a
// test problem. 0xF3EExxxx names the failed step ("FREE GOOD" when whole).
#define MEMORY_TEST_RETVAL 0xF3EE600DUL

static bool test_ring3_memory(void)
{
    if (kRootFilesystem == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_ring3_memory (no root filesystem mounted)\n");
        return true;
    }

    task_t *task = task_create("/bin/memory_test", 0, NULL, kKernelTask, false, THREAD_NO_AFFINITY);
    if (task == NULL) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_memory - task_create returned NULL\n");
        return false;
    }

    scheduler_submit_new_task(task);

    // 64 demand-paged touches plus six syscalls: instant. 3s is pure slack.
    for (int i = 0; i < 300 && !task->exited; i++)
        wait(10);

    if (!task->exited) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_memory - task did not exit within 3 seconds\n");
        return false;
    }

    if (task->retVal != MEMORY_TEST_RETVAL) {
        printd(DEBUG_TESTS, "\tFAIL: test_ring3_memory - retVal=0x%lx, expected 0x%lx "
               "(0xF3EExxxx identifies the failed step; see test/elf/memory_test.c)\n",
               task->retVal, (uint64_t)MEMORY_TEST_RETVAL);
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_ring3_memory (fields sane, books balance at rest/mid-allocation/post-unmap)\n");
    return true;
}

// (A dedicated /bin/hello test lived here briefly during userland bring-up;
// removed as redundant — ring3_syscall_smoke and ring3_exit_by_return already
// cover load-run-exit at CPL 3, and the HELLO boot-flow launch exercises the
// real app path. /bin/hello stays on the image for that launch.)

// ── net tests ────────────────────────────────────────────────────────────────

// The driver's first round trip: hand-roll an ARP request ("who has
// 10.0.2.2? tell 10.0.2.15"), transmit it raw through the seam, and wait
// for the gateway's reply to come back up the RX path. This is a DRIVER
// test, not a protocol test — the real ARP lives in the Phase 2 stack;
// the hardcoded bytes here exist to prove TX-on-the-wire and RX-delivery
// with zero stack code in the loop. The 10.0.2.x constants are the NAT
// convention BOTH QEMU user-mode networking and VirtualBox NAT use
// (guest 10.0.2.15, gateway 10.0.2.2), so the same test serves both.
// (Historical note: in slice 1b this test predated the stack, so the
// reply landed on rx_dropped_no_handler. Now that ethernet.c claims the
// handler at boot, the reply is DELIVERED and counts in rx_frames — the
// assertion below watches the SUM of both on purpose, so it was true in
// both eras and stays a pure driver test either way.)
static bool test_net_wire(void)
{
    if (kNetDeviceCount == 0) {
        printd(DEBUG_TESTS, "\tSKIP: test_net_wire (no NIC — QEMU: -netdev user,id=n0 -device virtio-net-pci,netdev=n0)\n");
        return true;
    }

    net_device_t *dev = kNetDevices[0];
    uint64_t seen_before = dev->rx_frames + dev->rx_dropped_no_handler;
    uint64_t tx_before   = dev->tx_frames;

    // Ethernet (14 bytes) + ARP (28 bytes) = 42, the minimum-famous frame.
    // Network byte order is big-endian, so multi-byte fields are spelled
    // out byte-at-a-time — no htons() exists here, and per the NETWORK.md
    // ruling proposal, none ever will outside the kernel's wire layer.
    uint8_t f[42];
    int n = 0;
    memset(f, 0xFF, 6);                 n += 6;   // eth dst: broadcast
    memcpy(f + n, dev->mac, 6);         n += 6;   // eth src: us
    f[n++] = 0x08; f[n++] = 0x06;                 // ethertype: ARP
    f[n++] = 0x00; f[n++] = 0x01;                 // htype: ethernet
    f[n++] = 0x08; f[n++] = 0x00;                 // ptype: IPv4
    f[n++] = 6;    f[n++] = 4;                    // hlen, plen
    f[n++] = 0x00; f[n++] = 0x01;                 // oper: request
    memcpy(f + n, dev->mac, 6);         n += 6;   // sender MAC
    f[n++] = 10; f[n++] = 0; f[n++] = 2; f[n++] = 15;  // sender IP
    memset(f + n, 0x00, 6);             n += 6;   // target MAC: unknown (that's the question)
    f[n++] = 10; f[n++] = 0; f[n++] = 2; f[n++] = 2;   // target IP: the gateway

    int32_t rc = dev->ops->transmit(dev, f, sizeof(f));
    if (rc != 0) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_wire - transmit returned %d\n", rc);
        return false;
    }
    if (dev->tx_frames != tx_before + 1) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_wire - tx_frames did not advance\n");
        return false;
    }

    // The reply crosses a NAT stack in the host process — microseconds.
    // 2 seconds of patience is pure slack (the poll rides scheduler passes).
    for (int i = 0; i < 200; i++) {
        if (dev->rx_frames + dev->rx_dropped_no_handler > seen_before)
            break;
        wait(10);
    }

    if (dev->rx_frames + dev->rx_dropped_no_handler <= seen_before) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_wire - no frame came back within 2s "
               "(tx=%lu txerr=%lu rx=%lu drop_nh=%lu drop_big=%lu)\n",
               dev->tx_frames, dev->tx_errors, dev->rx_frames,
               dev->rx_dropped_no_handler, dev->rx_dropped_too_big);
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_net_wire (ARP request out, gateway reply counted at the seam)\n");
    return true;
}

// Phase 2, exhibit A — the stack's OWN ARP earns an answer: fire a real
// arp_send_request at the gateway, then watch the cache learn its MAC via
// the full inbound path (virtio poll → eth demux → arp_input → cache).
// The gateway may already be cached (the driver test above chats with it,
// and slirp itself may have ARPed us) — that's a pass too: "resolvable"
// is the property under test, not "was unresolved a moment ago".
static bool test_net_arp(void)
{
    if (kNetDeviceCount == 0) {
        printd(DEBUG_TESTS, "\tSKIP: test_net_arp (no NIC)\n");
        return true;
    }
    net_device_t *dev = kNetDevices[0];

    uint8_t mac[NET_MAC_LEN];
    if (!arp_lookup(kNetIPv4Gateway, mac)) {
        arp_send_request(dev, kNetIPv4Gateway);
        for (int i = 0; i < 200 && !arp_lookup(kNetIPv4Gateway, mac); i++)
            wait(10);
    }
    if (!arp_lookup(kNetIPv4Gateway, mac)) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_arp - gateway unresolved after 2s "
               "(req_sent=%lu rep_rcvd=%lu learned=%lu malformed=%lu)\n",
               kArpStats.requests_sent, kArpStats.replies_received,
               kArpStats.learned, kArpStats.malformed);
        return false;
    }

    // A resolved-to-nothing entry would mean we cached garbage.
    bool nonzero = false;
    for (int i = 0; i < NET_MAC_LEN; i++)
        if (mac[i]) { nonzero = true; break; }
    if (!nonzero) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_arp - cache returned an all-zero MAC\n");
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_net_arp (gateway %u.%u.%u.%u is at %02x:%02x:%02x:%02x:%02x:%02x)\n",
           NET_IPV4_OCTETS(kNetIPv4Gateway), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return true;
}

// Phase 2, exhibit B — os64 pings the world: three echo requests to the
// gateway, each one waited to completion (request built by icmp.c, wrapped
// by ipv4.c, framed by ethernet.c, DMA'd by virtio_net.c — and the reply
// climbing back up every one of those layers with checksums checked).
// The identifier 0x6F34 is "o4" — os64's initials on the wire.
static bool test_net_ping(void)
{
    if (kNetDeviceCount == 0) {
        printd(DEBUG_TESTS, "\tSKIP: test_net_ping (no NIC)\n");
        return true;
    }
    net_device_t *dev = kNetDevices[0];

    uint64_t got = kIcmpStats.echo_replies_received;
    for (uint16_t seq = 1; seq <= 3; seq++) {
        // -2 = "ARP still resolving, retry shortly" — the documented
        // first-packet behavior (see ipv4_send). The retry loop IS the
        // caller-side contract for it.
        int32_t rc = -2;
        for (int i = 0; i < 100 && rc == -2; i++) {
            rc = icmp_send_echo_request(dev, kNetIPv4Gateway, 0x6F34, seq);
            if (rc == -2)
                wait(10);
        }
        if (rc != 0) {
            printd(DEBUG_TESTS, "\tFAIL: test_net_ping - send seq %u returned %d\n", seq, rc);
            return false;
        }
        for (int i = 0; i < 200 && kIcmpStats.echo_replies_received <= got; i++)
            wait(10);
        if (kIcmpStats.echo_replies_received <= got) {
            printd(DEBUG_TESTS, "\tFAIL: test_net_ping - no reply to seq %u within 2s "
                   "(sent=%lu rcvd=%lu bad_cksum=%lu ipv4_rx=%lu)\n",
                   seq, kIcmpStats.echo_requests_sent, kIcmpStats.echo_replies_received,
                   kIcmpStats.bad_checksum, kIPv4Stats.rx_delivered);
            return false;
        }
        got = kIcmpStats.echo_replies_received;
    }

    if (kIcmpStats.last_reply_src != kNetIPv4Gateway ||
        kIcmpStats.last_reply_ident != 0x6F34 || kIcmpStats.last_reply_seq != 3) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_ping - last reply identity wrong "
               "(src=%u.%u.%u.%u id=0x%x seq=%u)\n",
               NET_IPV4_OCTETS(kIcmpStats.last_reply_src),
               kIcmpStats.last_reply_ident, kIcmpStats.last_reply_seq);
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_net_ping (3 echoes to %u.%u.%u.%u, 3 replies, id/seq verified)\n",
           NET_IPV4_OCTETS(kNetIPv4Gateway));
    return true;
}

// Phase 2, exhibit C — the RESPONDER halves, which slirp cannot exercise
// for us (user-mode NAT never pings the guest; hostfwd forwards only
// TCP/UDP — so "host pings os64" awaits real hardware or a tap netdev).
// Instead we impersonate a neighbor: hand-build the frames a real peer
// would send and INJECT them at the seam via net_device_rx, exactly as a
// driver would. The stack can't tell the difference — that's what a seam
// is — and its answers go out on the REAL wire, so the pcap holds the
// receipts: an ARP reply and an echo reply, addressed to a machine that
// never existed. Delivery is synchronous (net_device_rx calls straight
// up the stack), so there's nothing to wait for — inject, then look.
static bool test_net_echo_responder(void)
{
    if (kNetDeviceCount == 0) {
        printd(DEBUG_TESTS, "\tSKIP: test_net_echo_responder (no NIC)\n");
        return true;
    }
    net_device_t *dev = kNetDevices[0];

    // The ghost: a locally-administered MAC (0x02 first octet = "nobody
    // manufactured this") at .99 on our subnet.
    static const uint8_t ghost_mac[NET_MAC_LEN] = {0x02, 0x64, 0x0E, 0x0A, 0x05, 0x99};
    uint32_t ghost_ip = (kNetIPv4Address & kNetIPv4Netmask) | 99;

    // ── Part 1: the ARP responder. The ghost broadcasts "who has os64's
    // IP?" — the stack must learn the asker AND answer the question.
    uint8_t f[64];
    int n = 0;
    memset(f + n, 0xFF, NET_MAC_LEN);           n += NET_MAC_LEN;   // eth dst: broadcast
    memcpy(f + n, ghost_mac, NET_MAC_LEN);      n += NET_MAC_LEN;   // eth src: the ghost
    net_write16(f + n, ETH_TYPE_ARP);           n += 2;
    net_write16(f + n, 1);                      n += 2;             // htype ethernet
    net_write16(f + n, ETH_TYPE_IPV4);          n += 2;             // ptype IPv4
    f[n++] = NET_MAC_LEN; f[n++] = 4;                               // hlen, plen
    net_write16(f + n, ARP_OPER_REQUEST);       n += 2;
    memcpy(f + n, ghost_mac, NET_MAC_LEN);      n += NET_MAC_LEN;   // sender MAC
    net_write32(f + n, ghost_ip);               n += 4;             // sender IP
    memset(f + n, 0x00, NET_MAC_LEN);           n += NET_MAC_LEN;   // target MAC: the blank
    net_write32(f + n, kNetIPv4Address);        n += 4;             // target IP: us

    uint64_t arp_answers = kArpStats.replies_sent;
    uint64_t tx_before   = dev->tx_frames;
    net_device_rx(dev, f, (uint16_t)n);

    if (kArpStats.replies_sent != arp_answers + 1 || dev->tx_frames <= tx_before) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_echo_responder - ARP request in, no reply out "
               "(replies_sent=%lu malformed=%lu tx=%lu)\n",
               kArpStats.replies_sent, kArpStats.malformed, dev->tx_frames);
        return false;
    }
    uint8_t learned[NET_MAC_LEN];
    if (!arp_lookup(ghost_ip, learned) || memcmp(learned, ghost_mac, NET_MAC_LEN) != 0) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_echo_responder - sender not learned from its question\n");
        return false;
    }

    // ── Part 2: the ICMP echo responder — the arc's Phase 2 headline.
    // The ghost pings us: 16 bytes of 'A'..'P' payload, id 0xBEEF seq 7.
    n = 0;
    memcpy(f + n, dev->mac, NET_MAC_LEN);       n += NET_MAC_LEN;   // eth dst: us, unicast
    memcpy(f + n, ghost_mac, NET_MAC_LEN);      n += NET_MAC_LEN;
    net_write16(f + n, ETH_TYPE_IPV4);          n += 2;
    int ip_start = n;
    f[n++] = 0x45; f[n++] = 0x00;                                   // v4 ihl5, tos
    net_write16(f + n, 20 + ICMP_HDR_LEN + 16); n += 2;             // total length
    net_write16(f + n, 0x0007);                 n += 2;             // ident (ghost's choice)
    net_write16(f + n, 0x4000);                 n += 2;             // DF
    f[n++] = 64; f[n++] = IPV4_PROTO_ICMP;                          // ttl, proto
    int ip_cksum_at = n;
    net_write16(f + n, 0);                      n += 2;             // checksum placeholder
    net_write32(f + n, ghost_ip);               n += 4;
    net_write32(f + n, kNetIPv4Address);        n += 4;
    net_write16(f + ip_cksum_at, net_checksum(f + ip_start, 20));
    int icmp_start = n;
    f[n++] = ICMP_TYPE_ECHO_REQUEST; f[n++] = 0;
    int icmp_cksum_at = n;
    net_write16(f + n, 0);                      n += 2;
    net_write16(f + n, 0xBEEF);                 n += 2;             // ident
    net_write16(f + n, 7);                      n += 2;             // sequence
    for (int i = 0; i < 16; i++)
        f[n++] = (uint8_t)('A' + i);
    net_write16(f + icmp_cksum_at, net_checksum(f + icmp_start, ICMP_HDR_LEN + 16));

    uint64_t pings_seen = kIcmpStats.echo_requests_received;
    uint64_t echoes_out = kIcmpStats.echo_replies_sent;
    tx_before = dev->tx_frames;
    net_device_rx(dev, f, (uint16_t)n);

    if (kIcmpStats.echo_requests_received != pings_seen + 1 ||
        kIcmpStats.echo_replies_sent != echoes_out + 1 || dev->tx_frames <= tx_before) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_echo_responder - ping in, no echo out "
               "(req_rcvd=%lu rep_sent=%lu bad_cksum=%lu ipv4_trunc=%lu tx=%lu)\n",
               kIcmpStats.echo_requests_received, kIcmpStats.echo_replies_sent,
               kIcmpStats.bad_checksum, kIPv4Stats.rx_truncated, dev->tx_frames);
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_net_echo_responder (ARP reply + echo reply sent to an injected neighbor)\n");
    return true;
}

// Phase 3, exhibit A — the DHCP client's whole conversation, judged by its
// results. dhcp_start fired a DISCOVER during kernel_init; by the time
// postboot tests run, the lease should be BOUND and applied. This test
// waits (briefly) for the state machine to settle and then audits the
// books: state, the lease fields, the applied config, and — because every
// packet of that conversation rode the new UDP layer — the UDP counters
// double as the wire test for udp.c itself (checksummed DISCOVER/REQUEST
// out from 0.0.0.0, OFFER/ACK demuxed to port 68 in).
//
// (A hand-rolled DISCOVER probe test lived here for one slice, verified
// first-boot, then retired: the real client claims port 68 for the life
// of the system — a port is a mailbox — and asserts everything the probe
// did and more.)
static bool test_net_dhcp(void)
{
    if (kNetDeviceCount == 0) {
        printd(DEBUG_TESTS, "\tSKIP: test_net_dhcp (no NIC)\n");
        return true;
    }

    // The transaction usually settles in the first few scheduler passes;
    // 5s covers all four 2s-spaced retries of a sleepy server.
    for (int i = 0; i < 500 && kDhcpStats.state != DHCP_BOUND
                            && kDhcpStats.state != DHCP_GAVE_UP; i++)
        wait(10);

    if (kDhcpStats.state != DHCP_BOUND) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_dhcp - not BOUND (state=%u disc=%lu off=%lu req=%lu ack=%lu nak=%lu udp_rx=%lu bad_ck=%lu)\n",
               (uint32_t)kDhcpStats.state, kDhcpStats.discovers_sent,
               kDhcpStats.offers_received, kDhcpStats.requests_sent,
               kDhcpStats.acks_received, kDhcpStats.naks_received,
               kUdpStats.rx_delivered, kUdpStats.rx_bad_checksum);
        return false;
    }

    // The lease must be real AND applied — a BOUND state whose config
    // didn't stick would be the worst kind of green light.
    if (kDhcpStats.lease_ip == 0 || kNetIPv4Address != kDhcpStats.lease_ip) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_dhcp - lease %u.%u.%u.%u not applied (addr=%u.%u.%u.%u)\n",
               NET_IPV4_OCTETS(kDhcpStats.lease_ip), NET_IPV4_OCTETS(kNetIPv4Address));
        return false;
    }
    if (kDhcpStats.offers_received == 0 || kDhcpStats.acks_received == 0) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_dhcp - BOUND without the conversation (off=%lu ack=%lu)\n",
               kDhcpStats.offers_received, kDhcpStats.acks_received);
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_net_dhcp (leased %u.%u.%u.%u/%u.%u.%u.%u gw %u.%u.%u.%u from %u.%u.%u.%u, %u s)\n",
           NET_IPV4_OCTETS(kDhcpStats.lease_ip), NET_IPV4_OCTETS(kDhcpStats.lease_mask),
           NET_IPV4_OCTETS(kDhcpStats.lease_gateway), NET_IPV4_OCTETS(kDhcpStats.lease_server),
           kDhcpStats.lease_seconds);
    return true;
}

// Phase 3 finale, exhibit A — the conversation object, judged deterministically.
// Dials the ghost neighbor from the responder test (whose MAC that test
// already taught our ARP cache — registration order is load-bearing), then
// plays the peer's half by hand: frames injected at the seam, exactly as
// the responder test pioneered. Covers the conn machinery no live network
// can probe on demand: the connected-peer filter, the truncation contract,
// and the queue running while a reader drains it.
static bool test_net_udp_conn(void)
{
    if (kNetDeviceCount == 0) {
        printd(DEBUG_TESTS, "\tSKIP: test_net_udp_conn (no NIC)\n");
        return true;
    }
    net_device_t *dev = kNetDevices[0];
    static const uint8_t ghost_mac[NET_MAC_LEN] = {0x02, 0x64, 0x0E, 0x0A, 0x05, 0x99};
    uint32_t ghost_ip = (kNetIPv4Address & kNetIPv4Netmask) | 99;

    udp_conn_t *conn = udp_conn_dial(dev, ghost_ip, 5555);
    if (conn == NULL) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_udp_conn - dial failed\n");
        return false;
    }

    // Outbound: one datagram to the ghost (its MAC is cached; the write's
    // ARP retry path stays cold). Just the plumbing, counted at UDP.
    uint64_t udp_tx = kUdpStats.tx_sent;
    if (udp_conn_write(conn, "knock knock", 11) != 11 || kUdpStats.tx_sent != udp_tx + 1) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_udp_conn - write failed (udp tx=%lu)\n", kUdpStats.tx_sent);
        udp_conn_close(conn);
        return false;
    }

    // Build one inbound frame from the ghost: eth + ipv4 + udp, correct
    // checksums, payload as given. Reused for all three injections below.
    uint8_t f[128];
    const char *msg1 = "os64 hears you";       // 14 bytes
    #define CONN_FRAME(src_port, payload, plen) do {                          \
        int n = 0;                                                            \
        memcpy(f + n, dev->mac, NET_MAC_LEN);      n += NET_MAC_LEN;          \
        memcpy(f + n, ghost_mac, NET_MAC_LEN);     n += NET_MAC_LEN;          \
        net_write16(f + n, ETH_TYPE_IPV4);         n += 2;                    \
        int ip_start = n;                                                     \
        f[n++] = 0x45; f[n++] = 0x00;                                         \
        net_write16(f + n, 20 + UDP_HDR_LEN + (plen)); n += 2;                \
        net_write16(f + n, 0x0042);                n += 2;                    \
        net_write16(f + n, 0x4000);                n += 2;                    \
        f[n++] = 64; f[n++] = IPV4_PROTO_UDP;                                 \
        int ip_ck = n; net_write16(f + n, 0);      n += 2;                    \
        net_write32(f + n, ghost_ip);              n += 4;                    \
        net_write32(f + n, kNetIPv4Address);       n += 4;                    \
        net_write16(f + ip_ck, net_checksum(f + ip_start, 20));               \
        net_write16(f + n, (src_port));            n += 2;                    \
        net_write16(f + n, conn->local_port);      n += 2;                    \
        net_write16(f + n, UDP_HDR_LEN + (plen));  n += 2;                    \
        net_write16(f + n, 0);                     n += 2;  /* cksum 0 = none (IPv4-legal) */ \
        memcpy(f + n, (void*)(payload), (plen));   n += (plen);               \
        net_device_rx(dev, f, (uint16_t)n);                                   \
    } while (0)

    // In from the PEER: must queue and read back verbatim.
    CONN_FRAME(5555, msg1, 14);
    char buf[64];
    long got = udp_conn_read(conn, buf, sizeof(buf), 0);
    if (got != 14 || memcmp(buf, msg1, 14) != 0) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_udp_conn - readback got %ld (delivered=%lu)\n",
               got, conn->rx_delivered);
        udp_conn_close(conn);
        return false;
    }

    // In from a STRANGER (right IP, wrong port): the connected filter must
    // drop it on its named counter and queue nothing.
    uint64_t strangers = conn->rx_dropped_stranger;
    CONN_FRAME(6666, "impostor", 8);
    if (conn->rx_dropped_stranger != strangers + 1 || conn->count != 0) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_udp_conn - stranger filter leaked (dropped=%lu count=%u)\n",
               conn->rx_dropped_stranger, conn->count);
        udp_conn_close(conn);
        return false;
    }

    // Truncation contract: a 14-byte datagram read into an 8-byte buffer
    // returns 8 and the tail DROPS — one datagram, one read, no carryover.
    CONN_FRAME(5555, msg1, 14);
    got = udp_conn_read(conn, buf, 8, 0);
    if (got != 8 || conn->count != 0) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_udp_conn - truncation contract broken (got %ld, queued %u)\n",
               got, conn->count);
        udp_conn_close(conn);
        return false;
    }
    #undef CONN_FRAME

    // The deadline contract (os64_read_for's kernel half): an EMPTY queue
    // plus an expired deadline returns TIMEOUT — after actually waiting.
    // Deterministic by construction: nothing sends to this ephemeral port.
    // Budget: 10-tick deadline, elapsed must land in [10, 40] — the upper
    // bound generous because the wake rides the sweep + backstop lattice.
    uint64_t t0 = kTicksSinceStart;
    got = udp_conn_read(conn, buf, sizeof(buf), kTicksSinceStart + 10);
    uint64_t waited = kTicksSinceStart - t0;
    if (got != UDP_CONN_ERR_TIMEOUT || waited < 10 || waited > 40) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_udp_conn - deadline read got %ld after %lu ticks "
               "(want UDP_CONN_ERR_TIMEOUT in [10,40])\n", got, waited);
        udp_conn_close(conn);
        return false;
    }

    udp_conn_close(conn);
    printd(DEBUG_TESTS, "\tPASS: test_net_udp_conn (dial, write, filtered+truncated reads, deadline, hangup)\n");
    return true;
}

// Phase 3 finale, exhibit B — ring 3 places a real call. Spawns
// /bin/dialtest, which dials slirp's DNS (10.0.2.3:53) with os64_dial's
// bang string, asks a genuine question, and BLOCKS in read until the
// answer crosses two NATs and comes home — the full tower, syscall to
// wire to park to wake, judged by one exit code. See dialtest.c for the
// 0x0D1A16xx step-code autopsy table.
static bool test_net_dial_ring3(void)
{
    if (kNetDeviceCount == 0) {
        printd(DEBUG_TESTS, "\tSKIP: test_net_dial_ring3 (no NIC)\n");
        return true;
    }
    if (kRootFilesystem == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_net_dial_ring3 (no root filesystem)\n");
        return true;
    }

    task_t *task = task_create("/bin/dialtest", 0, NULL, kKernelTask, false, THREAD_NO_AFFINITY);
    if (task == NULL) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_dial_ring3 - task_create failed\n");
        return false;
    }
    scheduler_submit_new_task(task);

    // DNS through slirp is normally milliseconds; 5s covers a resolver
    // having a bad day. (A host with NO resolver at all fails here — that
    // is a finding about the host, and the step code will say BAD_READ.)
    for (int i = 0; i < 500 && !task->exited; i++)
        wait(10);

    if (!task->exited) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_dial_ring3 - fixture still blocked after 5s "
               "(udp tx=%lu rx=%lu)\n", kUdpStats.tx_sent, kUdpStats.rx_delivered);
        return false;
    }
    if (task->retVal != 0x0D1A1600UL) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_dial_ring3 - retVal=0x%lx, expected 0x0D1A1600 "
               "(step codes in dialtest.c)\n", task->retVal);
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_net_dial_ring3 (ring 3 dialed DNS, asked, was answered)\n");
    return true;
}

// The ICMP handle — the mechanism `ping` is waiting on (utilities are
// Chris's; the kernel plumbing is mine). Dials the gateway, sends a
// payload carrying a tick stamp, and reads the echo back: proving the
// identifier demux, the payload round trip, and the blocking read that
// `ping` will time with os64_ticks(). Live against slirp, which answers
// echo for its gateway address (test_net_ping already relies on that).
static bool test_net_icmp_conn(void)
{
    if (kNetDeviceCount == 0) {
        printd(DEBUG_TESTS, "\tSKIP: test_net_icmp_conn (no NIC)\n");
        return true;
    }

    icmp_conn_t *c = icmp_conn_dial(kNetDevices[0], kNetIPv4Gateway);
    if (c == NULL) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_icmp_conn - dial failed\n");
        return false;
    }

    // The payload every ping since 1983 sends: a timestamp to subtract
    // when it comes home, plus a recognizable pattern after it.
    uint8_t out[32];
    uint64_t stamp = kTicksSinceStart;
    memcpy(out, &stamp, sizeof(stamp));
    for (int i = 8; i < 32; i++)
        out[i] = (uint8_t)(0x40 + i);

    if (icmp_conn_write(c, out, sizeof(out)) != (long)sizeof(out)) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_icmp_conn - write failed\n");
        icmp_conn_close(c);
        return false;
    }

    uint8_t in[64];
    long got = icmp_conn_read(c, in, sizeof(in), 0);
    uint64_t rtt = kTicksSinceStart - stamp;

    if (got != (long)sizeof(out)) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_icmp_conn - read returned %ld, expected %u\n",
               got, (uint32_t)sizeof(out));
        icmp_conn_close(c);
        return false;
    }
    // Byte-for-byte: an echo that alters the payload is not an echo.
    if (memcmp(in, out, sizeof(out)) != 0) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_icmp_conn - payload came back altered\n");
        icmp_conn_close(c);
        return false;
    }

    // The once-a-minute regression, on demand: flush the ARP cache so this
    // next echo starts from a cold neighbor table — exactly what the 60s
    // lazy TTL does to a long-running ping. The write must RIDE OUT the
    // re-resolution (the udp-style retry finally ported here), not fail.
    // Found by Chris's `ping -n 3600`: echo #22 died the moment the boot-
    // era gateway entry hit its TTL, and once a minute after that.
    arp_cache_flush();
    if (icmp_conn_write(c, out, sizeof(out)) != (long)sizeof(out)) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_icmp_conn - cold-cache write failed "
               "(the 60s-TTL ping regression is back)\n");
        icmp_conn_close(c);
        return false;
    }
    got = icmp_conn_read(c, in, sizeof(in), 0);
    icmp_conn_close(c);
    if (got != (long)sizeof(out)) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_icmp_conn - cold-cache echo read returned %ld\n", got);
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_net_icmp_conn (%ld bytes echoed by %u.%u.%u.%u in %lu ticks; "
           "cold-cache echo survived the ARP re-ask)\n",
           got, NET_IPV4_OCTETS(kNetIPv4Gateway), rtt);
    return true;
}

// Phase 4, exhibit A — the RST path, which needs nothing but a closed
// door. Dialing a port nobody listens on must FAIL FAST: slirp's host
// side refuses, the refusal comes back as a TCP reset, and tcp_input's
// RST arm turns it into a failed dial. This is the deterministic half of
// the TCP tests (no internet required) and it exercises the arm that a
// happy-path fetch never touches. Port 9 is "discard" (RFC 863) — a
// service nobody has run since the 1980s, which is exactly why it makes
// a dependable closed door.
static bool test_net_tcp_refused(void)
{
    if (kNetDeviceCount == 0) {
        printd(DEBUG_TESTS, "\tSKIP: test_net_tcp_refused (no NIC)\n");
        return true;
    }

    uint64_t refused_before = kTcpStats.connections_refused;
    uint64_t start = kTicksSinceStart;

    int64_t why = 0;
    tcp_conn_t *c = tcp_conn_dial(kNetDevices[0], kNetIPv4Gateway, 9, &why);
    uint64_t elapsed = kTicksSinceStart - start;

    if (c != NULL) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_tcp_refused - dial to a closed port SUCCEEDED\n");
        tcp_conn_close(c);
        return false;
    }
    // The dial must not just fail — it must fail with the RIGHT STORY.
    // A ping author staring at a bare -1 is how this assertion got here.
    if (why != OS64_NET_ERR_REFUSED) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_tcp_refused - failed with why=%ld, "
               "want OS64_NET_ERR_REFUSED (%d)\n", why, OS64_NET_ERR_REFUSED);
        return false;
    }
    // A refusal must be an ANSWER (an RST), not the connect timeout —
    // fast failure is the observable difference, and the timeout is 10s.
    if (kTcpStats.connections_refused != refused_before + 1) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_tcp_refused - no RST counted "
               "(refused=%lu timeouts=%lu resets=%lu, took %lu ticks)\n",
               kTcpStats.connections_refused, kTcpStats.connect_timeouts,
               kTcpStats.resets_received, elapsed);
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_net_tcp_refused (closed port answered with RST in %lu ticks)\n",
           elapsed);
    return true;
}

// Phase 4, exhibit B — THE MILESTONE. Spawns /bin/fetchtest, which
// resolves example.com over UDP, opens a TCP stream to it through slirp,
// speaks HTTP, and reads the page to EOF. Everything in os64 that touches
// a network is in the path: handshake, sequence arithmetic, ACKs, the
// receive ring, the FIN that becomes read()'s 0. Needs working internet
// on the host — the one test here that does, and it says so when it fails.
// See fetchtest.c for the 0x0FE7C4xx step codes.
static bool test_net_tcp_fetch_ring3(void)
{
    if (kNetDeviceCount == 0) {
        printd(DEBUG_TESTS, "\tSKIP: test_net_tcp_fetch_ring3 (no NIC)\n");
        return true;
    }
    if (kRootFilesystem == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_net_tcp_fetch_ring3 (no root filesystem)\n");
        return true;
    }

    task_t *task = task_create("/bin/fetchtest", 0, NULL, kKernelTask, false, THREAD_NO_AFFINITY);
    if (task == NULL) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_tcp_fetch_ring3 - task_create failed\n");
        return false;
    }
    scheduler_submit_new_task(task);

    // DNS + a TCP round trip to the real internet: normally under a
    // second, 15s of slack for a sleepy CDN or a retransmit or two.
    for (int i = 0; i < 1500 && !task->exited; i++)
        wait(10);

    if (!task->exited) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_tcp_fetch_ring3 - fixture still running after 15s "
               "(tcp opened=%lu refused=%lu timeouts=%lu retrans=%lu)\n",
               kTcpStats.connections_opened, kTcpStats.connections_refused,
               kTcpStats.connect_timeouts, kTcpStats.retransmits);
        return false;
    }
    if (task->retVal != 0x0FE7C400UL) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_tcp_fetch_ring3 - retVal=0x%lx, expected 0x0FE7C400 "
               "(step codes in fetchtest.c; needs host internet)\n", task->retVal);
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_net_tcp_fetch_ring3 (fetched a real page from the real internet, "
           "%lu segments in / %lu out, %lu retransmits)\n",
           kTcpStats.segments_in, kTcpStats.segments_out, kTcpStats.retransmits);
    return true;
}

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
    test_register("ring3_file_io_concurrent", test_ring3_file_io_concurrent, TEST_PHASE_POSTBOOT);
    test_register("ring3_redirect_io", test_ring3_redirect_io, TEST_PHASE_POSTBOOT);
    test_register("ring3_dir_list", test_ring3_dir_list, TEST_PHASE_POSTBOOT);
    test_register("ext2_real_partition", test_ext2_real_partition, TEST_PHASE_POSTBOOT);
    test_register("mount_table", test_mount_table, TEST_PHASE_POSTBOOT);
    test_register("ring3_map_unmap", test_ring3_map_unmap, TEST_PHASE_POSTBOOT);
    test_register("ring3_cwd", test_ring3_cwd, TEST_PHASE_POSTBOOT);
    test_register("ring3_stat", test_ring3_stat, TEST_PHASE_POSTBOOT);
    test_register("ring3_sleep", test_ring3_sleep, TEST_PHASE_POSTBOOT);
    test_register("ring3_memory", test_ring3_memory, TEST_PHASE_POSTBOOT);
    test_register("net_wire", test_net_wire, TEST_PHASE_POSTBOOT);
    test_register("net_arp", test_net_arp, TEST_PHASE_POSTBOOT);
    test_register("net_ping", test_net_ping, TEST_PHASE_POSTBOOT);
    test_register("net_echo_responder", test_net_echo_responder, TEST_PHASE_POSTBOOT);
    test_register("net_dhcp", test_net_dhcp, TEST_PHASE_POSTBOOT);
    test_register("net_udp_conn", test_net_udp_conn, TEST_PHASE_POSTBOOT);
    test_register("net_dial_ring3", test_net_dial_ring3, TEST_PHASE_POSTBOOT);
    test_register("net_icmp_conn", test_net_icmp_conn, TEST_PHASE_POSTBOOT);
    test_register("net_tcp_refused", test_net_tcp_refused, TEST_PHASE_POSTBOOT);
    test_register("net_tcp_fetch_ring3", test_net_tcp_fetch_ring3, TEST_PHASE_POSTBOOT);
    test_register("vfs_write_mkdir", test_vfs_write_mkdir, TEST_PHASE_POSTBOOT);
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
            // PERMANENT screen print, not just serial: on a machine with no
            // COM port (the P5), a failure whose name only went to serial is
            // a confession sealed in an envelope. The panic below halts the
            // system anyway — the name must be readable on the glass.
            printf("  FAIL: %s\n", name);
        }
    }

    printd(DEBUG_TESTS, "BUILT-IN TESTS: %u passed, %u failed\n", (unsigned int)passed, (unsigned int)failed);

    // ONE summary line on the glass per phase — not the per-test chatter,
    // which stays on serial where it can be forty lines long without
    // eating the boot screen. This exists because success used to print
    // NOTHING here: the display said "Running post-boot tests ..." and
    // then moved on, so a suite that silently failed to run looked
    // exactly like a suite that passed. Failures already print by name
    // above (and panic); this is the other half of that honesty — the
    // count is what proves the tests actually happened.
    // (Chris caught it 2026-08-01, one slice after catching the same
    // "assume it all went to plan" habit in the DNS fixture.)
    printf("%s tests: %u passed, %u failed\n", label,
           (unsigned int)passed, (unsigned int)failed);

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
