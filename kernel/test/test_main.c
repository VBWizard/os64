#include "test_framework.h"
#include "panic.h"
#include "BasicRenderer.h"   // printf — failing test NAMES go on the glass (P5 has no serial)

#include "memory/kmalloc.h"
#include "memory/memset.h"
#include "memory/memcmp.h"
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
#include "smp.h"       // kCPUInfo/kMPCoreCount — the backstop test picks its AP
#include "signals.h"   // SIGKILL — delivered to the hog by a backstop pass
#include "CONFIG.h"    // the backstop test's windows are denominated in
                       // leases (kSchedBackstopMS), never in milliseconds
#include "task.h"
#include "scheduler.h"
#include "time.h"
#include "kernel.h"   // kTicksSinceStart — the TCP tests time their failures
#include "driver/filesystem/vfs/vfs.h"
#include "driver/filesystem/ext2/ext2_vfs.h"   // ext2_fops/ext2_dops (real-partition test)
#include "driver/block/block_cache.h"          // stats + is_active (buffer-cache test)
#include "shared_object.h"
#include "env.h"
#include "sprintf.h"
#include "console.h"   // console_read_deadline — the read-patience test
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
#include "fpu.h"

extern volatile uint64_t kTicksSinceStart;
extern volatile uint64_t kPageFaultCount;
extern task_t *kKernelTask;
extern vfs_filesystem_t *kRootFilesystem;
// The backstop test's witnesses (scheduler.c / smp.h / kernel.c).
extern volatile uint64_t kSchedBackstopFires[];
extern bool kSchedBackstopEnabled;
extern bool kTicklessScheduler;
extern int  kSchedBackstopMS;

// The TESTS= cmdline knob (parsed in kernel_commandline.c, consumed in
// test_run_phase): "panic" | "warn" override every test's registered
// failure policy for one boot; empty honors the registrations.
char kTestsPolicyOverride[8] = "";

static test_case_t g_test_cases[TEST_MAX_CASES];
static size_t g_test_case_count = 0;
static bool g_framework_initialized = false;

bool test_vma_file_backed_page_fault_resolved(void);
bool test_vma_partial_page_bss_zero_filled(void);

bool test_register_policy(const char *name, bool (*func)(void), int phase,
                          test_policy_t policy)
{
    if (g_test_case_count >= TEST_MAX_CASES) {
        const char *test_name = name ? name : "<unnamed>";
        printd(DEBUG_TESTS, "[Test] Failed to register %s (capacity reached)\n", test_name);
        return false;
    }

    g_test_cases[g_test_case_count].name = name;
    g_test_cases[g_test_case_count].func = func;
    g_test_cases[g_test_case_count].phase = phase;
    g_test_cases[g_test_case_count].policy = policy;
    g_test_case_count++;
    return true;
}

bool test_register(const char *name, bool (*func)(void), int phase)
{
    // The phase carries the default severity: preboot tests guard kernel
    // invariants (a failure impeaches the substrate — PANIC), postboot
    // tests mostly judge content and behavior (a failure earns analysis,
    // not a funeral — WARN). Tests that mean more say so via
    // test_register_policy.
    return test_register_policy(name, func, phase,
        phase == TEST_PHASE_PREBOOT ? TEST_POLICY_PANIC : TEST_POLICY_WARN);
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

static bool test_fpu_state_round_trip(void)
{
	// Preboot runs only on the BSP before the scheduler starts. Static storage
	// keeps these FXSAVE targets 16-byte aligned despite this kernel's current
	// early-boot stack alignment.
	static fpu_state_t original, first, second, observed;
	const uint64_t first_pattern[2] = { 0x0123456789ABCDEFULL, 0x0FEDCBA987654321ULL };
	const uint64_t second_pattern[2] = { 0xDEADBEEFCAFEBABEULL, 0x1122334455667788ULL };

	fpu_save(&original);
	fpu_state_init(&first);
	fpu_state_init(&second);
	fpu_restore(&first);
	__asm__ volatile("movdqu xmm0, %0" : : "m"(first_pattern) : "memory");
	fpu_save(&first);
	fpu_restore(&second);
	__asm__ volatile("movdqu xmm0, %0" : : "m"(second_pattern) : "memory");
	fpu_save(&second);
	fpu_restore(&first);
	fpu_save(&observed);
	if (memcmp(&observed.bytes[160], first_pattern, sizeof(first_pattern)) != 0)
	{
		fpu_restore(&original);
		TEST_FAIL("FXSAVE did not restore the first XMM0 pattern");
	}
	fpu_restore(&second);
	fpu_save(&observed);
	if (memcmp(&observed.bytes[160], second_pattern, sizeof(second_pattern)) != 0)
	{
		fpu_restore(&original);
		TEST_FAIL("FXSAVE did not restore the second XMM0 pattern");
	}
	fpu_restore(&original);
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

    // Unmapped before, mapped after — see the long note in
    // test_vma_file_backed.c. The old form watched the GLOBAL fault counter
    // for a delta of exactly one, which any concurrently running task
    // (/bin/logd faulting in its own ELF, say) turns into a false failure.
    uintptr_t before = paging_walk_paging_table((pt_entry_t *)task->pml4v, test_addr);
    bool wasUnmapped = (before == 0 || before == 0xbadbadba);

    volatile uint32_t *ptr = (volatile uint32_t *)test_addr;
    *ptr = 0xBEEFCAFE; // Should trigger page fault and be resolved

    uintptr_t after = paging_walk_paging_table((pt_entry_t *)task->pml4v, test_addr);
    bool ok = wasUnmapped && (after != 0 && after != 0xbadbadba) && (*ptr == 0xBEEFCAFE);

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

// Exhaustion is no longer a failure — it is a GROWTH trigger (2026-08-12,
// the paging-arena work; arena.h carries the contract). This test asserted
// NULL-on-exhaustion for the old fixed arena and correctly panicked the boot
// the moment growth landed; now it asserts the things growth must actually
// guarantee — the new allocation lands, the OLD block's data survives on the
// chain, and reset/destroy return the chain without incident.
static bool test_arena_growth(void)
{
    arena_t *arena = arena_create(100);
    if (arena == NULL) {
        TEST_FAIL("arena_create returned NULL");
    }

    // Fill the birth capacity exactly, and stamp it — growth must not lose it.
    uint8_t *ptr1 = arena_alloc(arena, 100);
    if (ptr1 == NULL) {
        TEST_FAIL("arena_alloc failed for exact capacity");
    }
    ptr1[0] = 0xA5;
    ptr1[99] = 0x5A;

    // Past capacity: the arena must GROW, not refuse.
    uint8_t *ptr2 = arena_alloc(arena, 200);
    if (ptr2 == NULL) {
        TEST_FAIL("arena_alloc should GROW when exhausted, not return NULL");
    }
    if (arena->next == NULL) {
        TEST_FAIL("growth should chain the retired block on arena->next");
    }
    if (ptr1[0] != 0xA5 || ptr1[99] != 0x5A) {
        TEST_FAIL("growth must not disturb the retired block's contents");
    }
    ptr2[199] = 0xEE;   // the grown block must actually be writable memory

    // Aligned growth too — the table-page pattern (4KB at 4KB).
    void *ptr3 = arena_alloc_aligned(arena, 4096, 4096);
    if (ptr3 == NULL || ((uintptr_t)ptr3 & 0xFFF) != 0) {
        TEST_FAIL("arena_alloc_aligned across growth returned NULL or misaligned");
    }

    // Reset returns the chain and starts over at birth size.
    arena_reset(arena);
    if (arena->next != NULL) {
        TEST_FAIL("arena_reset should free the growth chain");
    }
    void *ptr4 = arena_alloc(arena, 50);
    if (ptr4 == NULL) {
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

// Spawn a test fixture: task_create with the corpse COLLECTED BY DECREE
// (autoReap). Every fixture below reads exited/retVal by direct poll off the
// task struct — nobody ever calls task_wait on them, and ktask (the parent)
// has no wait loop. Without the decree each fixture would sit in the
// graveyard as an unwaited zombie forever (the "16 zombies at boot" census,
// 2026-08-06); with it, kworker buries them. TIMING CONTRACT: a fixture must
// finish reading its child's struct within one kworker period (2s) of the
// child's death — every poll loop here reads retVal microseconds after
// `exited` flips, and the undertaker's two-phase burial adds another full
// period of grace on top, so the margin is >4s against a µs consumer.
// Deliberately NOT a blanket parent==ktask rule: husk/logd/kworker are also
// ktask children, and kForegroundTask must never point at a buried shell.
static task_t *test_spawn(char *path, int argc, char **argv, bool isKernelTask)
{
    // autoReap is NOT set here — and that is the whole fix (2026-08-09).
    //
    // It used to be set at BIRTH, which handed kworker a licence to free this
    // struct the instant the child died, while the fixture below was still
    // polling `exited` and `retVal` off it. os64 zeroes every allocation, so a
    // struct freed and recycled mid-poll reads `exited == 0` — indistinguishable
    // from "the child never finished". That is what "task did not exit within 5
    // seconds" actually meant, on a kernel that was working perfectly: measured
    // 2026-08-09, 8 of 10 periodic boots failed at least one spawn test, and on
    // the SAME boots the identical fixtures passed through os64_wait in ring 3.
    // Weeks of "periodic is unstable" was this.
    //
    // The comment on the death certificate in task.h had the protocol right all
    // along: "Direct-poll consumers signal the same thing through autoReap
    // AFTER their last read." So: spawn here, read freely (an uncollected
    // corpse with no waiting parent is invisible to kworker's sweep, so nothing
    // can free it), and call test_release() when finished. Every caller MUST
    // release on EVERY exit path or the corpse becomes a permanent zombie —
    // the one hazard this trades for, and a loud one, since `ps` shows it.
    return task_create(path, argc, argv, kKernelTask, isKernelTask, THREAD_NO_AFFINITY);
}

// The other half of test_spawn: the fixture's last act, releasing the corpse
// for burial now that nothing will read it again. Safe on NULL so error paths
// can call it unconditionally.
static void test_release(task_t *t)
{
    if (t != NULL)
        t->autoReap = true;
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

    task_t *elf_task = test_spawn("/tests/test_elf", 0, NULL, true);
    if (elf_task == NULL) {
        printd(DEBUG_TESTS, "\tFAIL: test_elf_loader - task_create returned NULL\n");
        return false;
    }

    scheduler_submit_new_task(elf_task);

    // Poll until the task exits or we time out. 5 seconds, not 1: the loop
    // exits the moment the task does, so a healthy boot never feels the
    // difference — but under SCHED=periodic with few cores a freshly
    // spawned task can wait whole slices for its first dispatch, and this
    // family's 1s deadline made elf_loader the suite's flakiest test
    // (2-core periodic boots halted the OS on it; 4 cores passed).
    for (int i = 0; i < 500 && !elf_task->exited; i++)
        wait(10);

    // SINGLE EXIT from here down, so the corpse is released exactly once on
    // every path (2026-08-09). A `return` that skipped test_release() would
    // strand an uncollected zombie for the life of the boot — visible in `ps`,
    // which is the loud failure this shape trades for the silent one it
    // replaces.
    bool ok = false;

    if (!elf_task->exited) {
        printd(DEBUG_TESTS, "\tFAIL: test_elf_loader - task did not exit within 5 seconds\n");
    }
    // The test ELF spans two pages: _start on page 1 (0x400000) and page2_func
    // on page 2 (0x401000).  At least two demand-page faults must have fired —
    // one per page.  This catches any regression of the vma->loaded-per-VMA bug
    // where the second fault in the same VMA would panic instead of mapping.
    //
    // THIS is why elf_loader keeps a kernel seat while its ring-3 half moved to
    // /tests/testrun: kPageFaultCount is a kernel global, and no program can see
    // it. The exit-code half is testrun's now; the demand-paging half is this.
    else if (kPageFaultCount < faults_before + 2) {
        printd(DEBUG_TESTS, "\tFAIL: test_elf_loader - expected >=2 page faults, got %lu\n",
               kPageFaultCount - faults_before);
    }
    else if (elf_task->retVal != ELF_TEST_RETVAL) {
        printd(DEBUG_TESTS, "\tFAIL: test_elf_loader - retVal=0x%lx, expected 0x%lx\n",
               elf_task->retVal, ELF_TEST_RETVAL);
    }
    else {
        printd(DEBUG_TESTS, "\tPASS: test_elf_loader (demand-paged, retVal correct, exited cleanly)\n");
        ok = true;
    }

    test_release(elf_task);   // last touch — kworker may bury it after this
    return ok;
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

    task_t *task_a = test_spawn("/tests/dyn_consumer", 0, NULL, true);
    if (task_a == NULL) {
        printd(DEBUG_TESTS, "\tFAIL: test_dynamic_linking - task_create (task A) returned NULL\n");
        return false;
    }
    scheduler_submit_new_task(task_a);

    task_t *task_b = test_spawn("/tests/dyn_consumer", 0, NULL, true);
    if (task_b == NULL) {
        printd(DEBUG_TESTS, "\tFAIL: test_dynamic_linking - task_create (task B) returned NULL\n");
        test_release(task_a);
        return false;
    }
    scheduler_submit_new_task(task_b);

    // Poll until both tasks exit or we time out (5s — the scheduling-delay
    // headroom rationale lives in test_elf_loader).
    for (int i = 0; i < 500 && (!task_a->exited || !task_b->exited); i++)
        wait(10);

    // EVERYTHING THE CORPSES CAN TELL US, GATHERED BEFORE THEY ARE RELEASED.
    // After test_release() the undertaker may bury either task at any moment,
    // and a recycled struct reads as zeros (2026-08-09; see test_spawn).
    //
    // This block used to be just the two retVals, on the reasoning — stated in
    // the old comment here — that "the registry checks further down touch no
    // task struct at all." That was not true, and had not been since burial
    // learned to free page tables: the physical-sharing check below walks
    // task_a->pml4v and task_b->pml4v, and arena_destroy hands those tables
    // back at burial. It survived on timing alone (burial is ≥2 kworker
    // periods away; the checks take microseconds) — a use-after-free that
    // never lost the race.
    //
    // The refcount checks joined it here for a second reason, 2026-08-13:
    // burial now RELEASES a task's reference (shared_object_release), so a
    // refcount read after test_release would be measuring how fast kworker is,
    // not whether dynamic linking works. Reading before the release makes the
    // expected values — 3 and 2 — hard guarantees again rather than lucky
    // ones, because nothing can be buried until autoReap is set.
    bool both_exited = task_a->exited && task_b->exited;
    uint64_t retval_a = task_a->retVal;
    uint64_t retval_b = task_b->retVal;

    // Registry state, captured live. (These lookups each take a reference that
    // is never released — deliberate: the test holds the registry's objects
    // warm for the rest of the boot, and the expected counts below include
    // them by name.)
    shared_object_t *exe_so = shared_object_load_or_get("/tests/dyn_consumer");
    shared_object_t *so     = shared_object_load_or_get("/lib/libtest.so");
    uint32_t exe_refcount   = (exe_so != NULL) ? exe_so->refcount : 0;
    uint32_t lib_refcount   = (so != NULL) ? so->refcount : 0;
    bool dep_scope_ok       = (exe_so != NULL && so != NULL &&
                               exe_so->dep_count == 1 && exe_so->deps[0] == so);

    // The physical-sharing walk, also while the tables are still standing.
    uintptr_t code_phys_a = 0, code_phys_b = 0;
    if (so != NULL) {
        // Page 0's RUNTIME address — shared_object_page_va, not load_bias by
        // itself. Identical for a library (vaddr_base is 0), and the two part
        // company for an executable, so use the helper everywhere rather than
        // leave a spelling here that is only accidentally right.
        uintptr_t lib_page0 = shared_object_page_va(so, 0);
        code_phys_a = paging_walk_paging_table((pt_entry_t *)task_a->pml4v, lib_page0);
        code_phys_b = paging_walk_paging_table((pt_entry_t *)task_b->pml4v, lib_page0);
    }

    test_release(task_a);
    test_release(task_b);

    if (!both_exited) {
        printd(DEBUG_TESTS, "\tFAIL: test_dynamic_linking - task(s) did not exit within 5 seconds\n");
        return false;
    }

    if (retval_a != DYN_CONSUMER_EXPECTED_PACKED) {
        printd(DEBUG_TESTS, "\tFAIL: test_dynamic_linking - task A retVal=0x%lx, expected 0x%lx\n",
               retval_a, (uint64_t)DYN_CONSUMER_EXPECTED_PACKED);
        return false;
    }

    if (retval_b != DYN_CONSUMER_EXPECTED_PACKED) {
        printd(DEBUG_TESTS, "\tFAIL: test_dynamic_linking - task B retVal=0x%lx, expected 0x%lx (CoW isolation broken?)\n",
               retval_b, (uint64_t)DYN_CONSUMER_EXPECTED_PACKED);
        return false;
    }

    // Both tasks must have found the SAME shared_object_t via the registry
    // (cache-hit path, not a fresh load each time). shared_object_load_or_get
    // bumps refcount on every call — direct lookups AND the internal
    // recursive loads of DT_NEEDED dependencies. The main executable is
    // requested once per task_create plus once above: A + B + this test = 3.
    // (Both tasks are still unburied at capture time, so neither has released
    // its edge yet — that is exactly what capturing before test_release buys.)
    if (exe_so == NULL) {
        printd(DEBUG_TESTS, "\tFAIL: test_dynamic_linking - dyn_consumer not found in registry after both tasks ran\n");
        return false;
    }
    if (exe_refcount != 3) {
        printd(DEBUG_TESTS, "\tFAIL: test_dynamic_linking - dyn_consumer refcount=%u, expected 3 (task A + task B + this lookup)\n",
               exe_refcount);
        return false;
    }

    // libtest.so, by contrast, is loaded as dyn_consumer's DT_NEEDED
    // dependency exactly ONCE — the second task_create cache-hits the
    // already-loaded executable and never re-walks its deps — so: that one
    // dependency edge + this lookup = 2. THIS ASYMMETRY IS LOAD-BEARING: it is
    // why the undertaker releases one edge on the main image rather than one
    // per entry in task->shared_objects, which would drive this very count
    // negative on the second burial (see shared_object.h's pairing rule).
    // It must also be exactly the object dyn_consumer's own dependency scope
    // points at (per-object symbol resolution — see shared_object.h's deps[]).
    if (so == NULL) {
        printd(DEBUG_TESTS, "\tFAIL: test_dynamic_linking - libtest.so not found in registry after both tasks ran\n");
        return false;
    }
    if (lib_refcount != 2) {
        printd(DEBUG_TESTS, "\tFAIL: test_dynamic_linking - libtest.so refcount=%u, expected 2 (dyn_consumer's dep edge + this lookup)\n",
               lib_refcount);
        return false;
    }
    if (!dep_scope_ok) {
        printd(DEBUG_TESTS, "\tFAIL: test_dynamic_linking - dyn_consumer's dependency scope doesn't point at the registry's libtest.so\n");
        return false;
    }

    // Task A and task B must share the SAME physical page backing
    // libtest.so's code segment — proving true cross-task physical sharing,
    // not silently-duplicated per-task copies — even though their .data
    // pages have since diverged via CoW. (Walked above, while both address
    // spaces were still standing.)
    if (code_phys_a == 0 || code_phys_a == 0xbadbadba || code_phys_a != code_phys_b) {
        printd(DEBUG_TESTS, "\tFAIL: test_dynamic_linking - libtest.so code page not physically shared (A=0x%lx, B=0x%lx)\n",
               code_phys_a, code_phys_b);
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_dynamic_linking (symbol resolution, relocation, CoW isolation, and physical sharing all correct)\n");
    return true;
}


// ── Retention and reload (2026-08-28) ────────────────────────────────────────
//
// Two claims, one fixture, because they are the two halves of one answer to
// "is the resident copy of a program still the program on disk?":
//
//   RETENTION — an object with no holders is UNLOADED, so the registry only
//   ever describes what is in use. Shown by loading a program, dropping the
//   reference, and finding the registry empty of it.
//
//   RELOAD — an object whose FILE has been replaced is retired and rebuilt
//   from the new file, while the old copy stays for whoever is still running
//   it. Shown by renaming a second copy over the first's name — which is
//   exactly how os64get commits a refresh — and finding a different object, a
//   different identity, and the old one retired but alive under its held
//   reference.
//
// The fixture is a byte copy of a real dynamically-linked program under a
// scratch name: the claims are about the REGISTRY, not about any particular
// binary, and /bin is not the suite's to rearrange.
#define SO_FAIL(...) do { \
        printd(DEBUG_TESTS, "\tFAIL: test_shared_object_reload - " __VA_ARGS__); \
        return false; \
    } while (0)

// Byte-copy `src` onto `dst` through the VFS. A real program is tens of
// kilobytes, so the buffer is kmalloc'd rather than a stack array.
static bool so_copy_file(const char *src, const char *dst)
{
    vfs_file_t *in = NULL, *out = NULL;
    if (kRootFilesystem->fops->open(&in, src, "r", kRootFilesystem) != 0)
        return false;
    if (kRootFilesystem->fops->open(&out, dst, "c", kRootFilesystem) != 0) {
        kRootFilesystem->fops->close(in);
        return false;
    }

    enum { SO_COPY_CHUNK = 8192 };
    uint8_t *buf = kmalloc(SO_COPY_CHUNK);
    bool ok = (buf != NULL);
    while (ok) {
        int n = kRootFilesystem->fops->read(in, buf, SO_COPY_CHUNK);
        if (n <= 0) {
            ok = (n == 0);   // 0 is end of file; negative is a real failure
            break;
        }
        if (kRootFilesystem->fops->write(out, buf, (size_t)n) != n)
            ok = false;
    }

    kfree(buf);
    kRootFilesystem->fops->close(in);
    kRootFilesystem->fops->close(out);
    return ok;
}

// Does the registry currently serve `path`? Asked the way every caller asks
// it — a retired object does not answer to its own name any more — and under
// the registry lock, because a walk without it can read a node another core
// is freeing (the same rule /sys/shlib follows).
static bool so_registry_serves(const char *path)
{
    bool found = false;
    shared_object_registry_lock();
    if (kLoadedSharedObjects != NULL) {
        for (dlist_node_t *n = kLoadedSharedObjects->head; n != NULL && !found; n = n->next) {
            shared_object_t *so = (shared_object_t *)n->data;
            found = (so != NULL && !so->retired && strcmp(so->path, path) == 0);
        }
    }
    shared_object_registry_unlock();
    return found;
}

static bool test_shared_object_reload(void)
{
    if (kRootFilesystem == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_shared_object_reload (no root filesystem mounted)\n");
        return true;
    }
    if (kRootFilesystem->fops->write == NULL || kRootFilesystem->fops->rename == NULL ||
        kRootFilesystem->fops->rm == NULL || kRootFilesystem->dops->mkdir == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_shared_object_reload (root filesystem cannot write, rename or remove)\n");
        return true;
    }

    static const char *live = "/etc/testdata/reload_a";
    static const char *incoming = "/etc/testdata/reload_b";

    // Provision, then start from a known floor — a previous boot's leftovers
    // would make "already loaded" read as a retention bug. (mkdir answers -1
    // for "already exists" too, so its result is deliberately ignored; the
    // copy below is the real judge of whether the directory is usable.)
    char pathbuf[40];
    sprintf(pathbuf, "%s", "/etc");
    kRootFilesystem->dops->mkdir(pathbuf, kRootFilesystem);
    sprintf(pathbuf, "%s", "/etc/testdata");
    kRootFilesystem->dops->mkdir(pathbuf, kRootFilesystem);
    kRootFilesystem->fops->rm(live, kRootFilesystem);
    kRootFilesystem->fops->rm(incoming, kRootFilesystem);

    if (!so_copy_file("/bin/hello", live))
        SO_FAIL("could not copy /bin/hello to %s\n", live);

    // 1. RETENTION. One load, one release, and nothing left behind.
    shared_object_t *first = shared_object_load_executable(live);
    if (first == NULL)
        SO_FAIL("could not load the copy at %s\n", live);
    uint64_t first_ident = first->ident;
    if (first_ident == 0)
        SO_FAIL("the root filesystem gave %s no identity — a replaced file could not be noticed\n", live);

    shared_object_release(first);   // the last reference: `first` is freed here
    if (so_registry_serves(live))
        SO_FAIL("%s outlived its last reference — unload-at-zero did not run\n", live);

    // 2. RELOAD. Hold one copy the way a running task would, replace the file
    //    underneath it, and ask for it again.
    shared_object_t *held = shared_object_load_executable(live);
    if (held == NULL)
        SO_FAIL("could not re-load %s after it was unloaded\n", live);
    uint64_t held_ident = held->ident;

    if (!so_copy_file("/bin/hello", incoming)) {
        shared_object_release(held);
        SO_FAIL("could not stage a replacement at %s\n", incoming);
    }
    if (kRootFilesystem->fops->rename(incoming, live, kRootFilesystem) != 0) {
        shared_object_release(held);
        SO_FAIL("could not rename %s over %s (an open destination must be replaceable)\n",
                incoming, live);
    }

    shared_object_t *fresh = shared_object_load_executable(live);
    if (fresh == NULL) {
        shared_object_release(held);
        SO_FAIL("loading %s after its file was replaced returned nothing\n", live);
    }

    // Everything the two objects can tell us, read BEFORE either release —
    // a release that drops the last reference frees the struct it names.
    bool distinct       = (fresh != held);
    bool identity_moved = (fresh->ident != held_ident);
    bool old_retired    = held->retired;
    uint32_t held_refs  = held->refcount;
    uint64_t fresh_ident = fresh->ident;

    shared_object_release(fresh);
    shared_object_release(held);

    if (!distinct)
        SO_FAIL("a replaced file returned the SAME object — the registry answered from the path alone\n");
    if (!identity_moved)
        SO_FAIL("the reloaded object kept identity %lu — the new file was not noticed\n", held_ident);
    if (!old_retired)
        SO_FAIL("the superseded object was not retired — a later load could still find it\n");
    if (held_refs != 1)
        SO_FAIL("the superseded object's refcount is %u, expected 1 (this test's own hold) — "
                "retiring must not disturb the count\n", held_refs);
    if (so_registry_serves(live))
        SO_FAIL("%s outlived both references\n", live);

    kRootFilesystem->fops->rm(live, kRootFilesystem);

    printd(DEBUG_TESTS, "\tPASS: test_shared_object_reload (unloaded at zero; a replaced file retired "
                        "the loaded copy, id %lu -> %lu, and reloaded)\n",
           held_ident, fresh_ident);
    return true;
}
#undef SO_FAIL


// ── Argument delivery (2026-08-13) ───────────────────────────────────────────
//
// /tests/arg_echo has existed since ring-3 bring-up and checks the whole startup
// contract end to end — argc in RDI, argv in RSI at TASK_ARGV_VIRT, env in RDX
// at TASK_ENV_VIRT, the argv strings actually copied into the task's own blob,
// the NULL terminator, and the ELF loader's partial-page BSS zero-fill — with a
// distinct 0xE00000xx code per broken invariant so a failure names itself.
//
// It was only ever run from /tests/testrun, the RING-3 suite, which lives behind
// its own Limine entry. So on an ordinary boot, argument processing had NO
// coverage at all: you could break argv and every normal boot would stay green.
// Chris asked for that gap to be closed (2026-08-13) the same evening the argv
// blob was about to be re-laid — packed strings, new fixed VAs, a 512-argument
// ceiling — which is exactly the change that would have broken it silently.
//
// Cheap: one spawn, no filesystem writes, and it fails with a number that says
// which invariant died rather than "the task exited wrong".
#define ARG_ECHO_OK 0x0A11600DUL
static bool test_task_args(void)
{
    if (kRootFilesystem == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_task_args (no root filesystem mounted)\n");
        return true;
    }

    // The exact argv the fixture asserts on — it checks argv[1]/argv[2] by
    // content, so these strings are part of the contract, not decoration.
    char *args[] = { "/tests/arg_echo", "hello", "world" };

    task_t *t = test_spawn("/tests/arg_echo", 3, args, false);
    if (t == NULL) {
        printd(DEBUG_TESTS, "\tFAIL: test_task_args - task_create returned NULL\n");
        return false;
    }
    scheduler_submit_new_task(t);

    for (int i = 0; i < 500 && !t->exited; i++)
        wait(10);

    // Read before releasing — after test_release the undertaker may free the
    // struct at any moment (2026-08-09; see test_spawn).
    bool exited = t->exited;
    uint64_t retval = t->retVal;
    test_release(t);

    if (!exited) {
        printd(DEBUG_TESTS, "\tFAIL: test_task_args - arg_echo did not exit within 5 seconds\n");
        return false;
    }
    if (retval != ARG_ECHO_OK) {
        printd(DEBUG_TESTS,
               "\tFAIL: test_task_args - arg_echo returned 0x%lx, expected 0x%lx. "
               "The code names the broken invariant (arg_echo.c): 1=argc 2=argv address "
               "3=argv NULLs 4=argv terminator 5..7=argv[0..2] contents 8=env address "
               "9=env empty A=.data init B=.bss zero-fill\n",
               retval, (uint64_t)ARG_ECHO_OK);
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_task_args (argc/argv/env delivered at the ABI addresses, "
                        "strings copied, BSS zero-filled)\n");
    return true;
}


// ── Environment growth (2026-08-14) ──────────────────────────────────────────
//
// The env block is born one page and grows on demand: when setenv fills it,
// syscall_setenv swaps a doubled block under the task's fixed TASK_ENV_VIRT
// window, up to the TASK_ENV_MAX_BYTES (64KB) ceiling. The dangerous moving
// parts are the mid-run REMAP of the task's own read-only window and the
// grow-copy chain preserving every pair — so the coverage is a ring-3 fixture
// (/tests/env_fill) that fills ~11KB (two growth events), reads every pair back
// through the remapped window, drives the block to the ceiling and demands an
// honest refusal, then proves the refusal corrupted nothing. Distinct
// 0xE27Fxxxx codes name the invariant that broke.
#define ENV_FILL_OK 0x0E27600DUL
static bool test_env_growth(void)
{
    if (kRootFilesystem == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_env_growth (no root filesystem mounted)\n");
        return true;
    }

    task_t *t = test_spawn("/tests/env_fill", 0, NULL, false);
    if (t == NULL) {
        printd(DEBUG_TESTS, "\tFAIL: test_env_growth - task_create returned NULL\n");
        return false;
    }
    scheduler_submit_new_task(t);

    // ~1000 setenv syscalls; generous 10s before calling it hung.
    for (int i = 0; i < 1000 && !t->exited; i++)
        wait(10);

    bool exited = t->exited;
    uint64_t retval = t->retVal;
    test_release(t);

    if (!exited) {
        printd(DEBUG_TESTS, "\tFAIL: test_env_growth - env_fill did not exit within 10 seconds\n");
        return false;
    }
    if (retval != ENV_FILL_OK) {
        printd(DEBUG_TESTS,
               "\tFAIL: test_env_growth - env_fill returned 0x%lx, expected 0x%lx. "
               "The code names the broken invariant (env_fill.c): 1=env ABI address "
               "2=fill-phase set failed 3=no growth 4=readback 5=PATH lost "
               "6=64KB ceiling never refused 7=refusal corrupted data "
               "8=replace-at-full 9=unset-frees-room\n",
               retval, (uint64_t)ENV_FILL_OK);
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_env_growth (block grew under the fixed window, pairs "
                        "survived the copies, 64KB ceiling refused honestly)\n");
    return true;
}


// ── The task-teardown leak test (2026-08-13; sharpened 2026-08-15) ──────────
//
// THE PROOF, now at full strength. As written on 8/13, teardown deliberately
// did not reclaim the VMA backing pages (the deferral ledger), so the test
// asserted the strongest thing then true: every byte a cycle consumed was
// either given back or COUNTED as deferred. The 8/13 comment here predicted:
// "when the refcount ruling lands and the pages start coming back, both
// sides fall to zero together and this test keeps passing without a line
// changing." The deferral was paid 2026-08-15 (the P5 status-table overrun
// was its bill — see task.c's ledger comment), both sides did fall to zero,
// and the test DID keep passing — but a zero-equals-zero pass is weaker than
// what is now true, so the assertion grew teeth the same day:
//
//     a spawn→exit→burial cycle costs NOTHING — allocator delta == 0 —
//     and the reclaim provably RAN: the undertaker freed at least the
//     fixture's guaranteed-per-task resident set, every cycle.
//
// The second clause is the regression guard the first can't provide: a
// demand-paging break that faulted nothing in would make the delta zero
// vacuously. Requiring kTaskVmaReclaimedBytes to climb by the fixture's
// floor proves pages were resident AND came back.
//
// The fixture matched the claim the same day (Chris's requirement: "a real
// program with real multiple text pages, bss, heap pages"): /tests/glutton
// deliberately touches ~4 text pages, a dirtied .data page, 4 .bss pages,
// and a 4-page mapped heap region it never unmaps — then exits with all of
// it resident, making burial do a real program's worth of work. The asserted
// floor counts only data+bss+heap (9 pages): those stay per-task under any
// future design, while text pages may one day be shared via the page cache —
// the executables-through-the-page-cache arc must not break this test.
//
// WHY POST-BOOT (Chris, 2026-08-13): "that's probably the only time there will
// just be one core running everything. Low complexity, nice and quiet." Exactly
// right, and it's the only such window that exists. The scheduler is up (burial
// needs kworker) but husk has not started, so nothing else is spawning, exiting,
// or churning the allocator underneath the measurement. Once the shell is live
// this becomes unmeasurable without heroics.
//
// THE WARM-UP ITERATION IS NOT A FUDGE. The first run of any program pays
// one-time costs that are caches, not leaks: block-cache lines for the binary,
// ext2 inode structures, the allocator's own status-table growth. Those are
// paid once and never again, so counting them as a per-task leak would be a
// lie in the other direction. Iteration 1 pays them; iterations 2..N measure
// the steady state, which is the number that actually tells you whether tasks
// accumulate.
#define TEARDOWN_LEAK_WARMUP_CYCLES    1
#define TEARDOWN_LEAK_MEASURED_CYCLES  2
#define GLUTTON_MAGIC                  0x0FEA57EDUL   // "FEASTED" (glutton.c)
// The reclaim floor per cycle: glutton's data(1) + bss(4) + heap(4) pages.
// Deliberately excludes its ~4 text pages — see the header comment.
#define GLUTTON_MIN_RECLAIM_PER_CYCLE  (9UL * 4096UL)

// Wait until the undertaker is idle: no completed burial for a settle window.
// Two kworker periods (2s each) plus slack, because burial is two-phase and a
// corpse unlinked in pass N is not freed until pass N+1 — a shorter window can
// see the gap BETWEEN a corpse's two phases and call it quiet.
//
// This runs before any measurement so the graveyard is empty when the clock
// starts: earlier tests (dynamic_linking releases two corpses immediately
// before this one) would otherwise be buried inside our window, and their
// frees would land in our arithmetic as memory we appeared to reclaim.
static bool teardown_leak_wait_quiet(void)
{
    uint64_t last = kTaskBurialCount;
    int quiet_ms = 0;

    for (int i = 0; i < 400 && quiet_ms < 5000; i++) {
        nap(50);
        if (kTaskBurialCount != last) {
            last = kTaskBurialCount;
            quiet_ms = 0;      // someone was buried — restart the settle
        } else {
            quiet_ms += 50;
        }
    }
    return quiet_ms >= 5000;
}

// AND THE OTHER HALF OF QUIET, which only became necessary when this test
// moved to the LATE phase (2026-08-29) and started running beside a live
// userland instead of on an empty machine.
//
// The settle above watches BURIALS, because burials were the only thing that
// moved memory while nothing but the suite was running. What the test
// actually asserts on is the GLOBAL free-page count, and that is moved by
// anything at all: logd draining into a file, a block cache warming, a shell
// faulting in a page it had not touched yet. None of those is a leak, and all
// of them land in `lost` as one.
//
// So ask the question the assertion depends on: is the free-page count STILL?
// Two samples a short pause apart, a few attempts, and a truthful "no" if it
// will not hold. A test that cannot measure must skip — it must never spend
// its caller's trust on a number it did not earn.
static bool teardown_leak_wait_allocation_still(void)
{
    for (int attempt = 0; attempt < 10; attempt++) {
        uint64_t a = 0, b = 0;
        allocator_memory_snapshot(&a, NULL, NULL);
        nap(200);
        allocator_memory_snapshot(&b, NULL, NULL);
        if (a == b)
            return true;
    }
    return false;
}

// One complete cycle: spawn a ring-3 fixture, let it exit, release the corpse,
// and WAIT FOR THE FUNERAL — not for the death. The distinction is the whole
// point: at exit time nothing has been freed yet, and a snapshot taken then
// would measure the corpse, not the cleanup.
//
// /tests/glutton is the fixture on purpose (since 2026-08-15; exit_by_return
// before that). It is ring 3, so it exercises every allocation burial frees —
// argv blob, env blob, exit trampoline page — AND it deliberately exits with
// a real program's resident set still mapped: multiple text pages, a dirtied
// .data page, four .bss pages, and a four-page heap region it never unmaps.
// exit_by_return proved the trampoline with a one-page footprint; a reclaim
// test needs a corpse with meat on it.
static bool teardown_leak_one_cycle(void)
{
    uint64_t burials_before = kTaskBurialCount;

    task_t *t = test_spawn("/tests/glutton", 0, NULL, false);
    if (t == NULL) {
        printd(DEBUG_TESTS, "\tFAIL: test_task_teardown_leak - task_create returned NULL\n");
        return false;
    }
    scheduler_submit_new_task(t);

    // nap(), not wait(): same tick cadence, but this waits on a task that has
    // to be SCHEDULED to make progress, and spinning here holds a core that
    // the fixture (and the shell beside it) could be using.
    for (int i = 0; i < 500 && !t->exited; i++)
        nap(10);

    // Read the struct BEFORE releasing it — after test_release the undertaker
    // may free it at any moment and a recycled struct reads as zeros (the
    // 2026-08-09 lesson; see test_spawn).
    bool exited = t->exited;
    uint64_t retval = t->retVal;
    test_release(t);

    if (!exited) {
        printd(DEBUG_TESTS, "\tFAIL: test_task_teardown_leak - fixture did not exit within 5 seconds\n");
        return false;
    }
    if (retval != GLUTTON_MAGIC) {
        printd(DEBUG_TESTS, "\tFAIL: test_task_teardown_leak - fixture retVal=0x%lx, expected 0x%lx. "
               "glutton.c names the broken step: 1=bss not zero 2=map failed "
               "3=heap not zero 4=data initializer wrong 5=readback\n",
               retval, (uint64_t)GLUTTON_MAGIC);
        return false;
    }

    // Now the funeral. Polling the census beats sleeping a guessed interval:
    // exact when the machine is fast, patient when it is slow. 15s ceiling is
    // ~7 kworker periods — if burial hasn't happened by then it isn't going to,
    // and that is itself the bug.
    //
    // nap(), not wait(): the cadence is the same tick, but this loop runs in
    // the LATE phase beside a live userland, and wait() spins.
    for (int i = 0; i < 1500 && kTaskBurialCount == burials_before; i++)
        nap(10);

    if (kTaskBurialCount == burials_before) {
        printd(DEBUG_TESTS, "\tFAIL: test_task_teardown_leak - no burial completed within 15 seconds "
                            "(corpse stuck: uncollected, or a thread never retired?)\n");
        return false;
    }
    return true;
}

// The verdict of ONE measurement attempt. The distinction that matters is
// between "the number is bad" and "the number is not mine to read", because
// only the first is a defect and only the second is worth retrying.
typedef enum {
    TL_CLEAN,     // measured, and a cycle cost nothing
    TL_DIRTY,     // could not get a window to measure in — say so, try again
    TL_ANOMALY,   // measured a loss (or a short reclaim) — REAL if it repeats
    TL_ERROR      // a cycle could not be run at all; nothing to do but fail
} teardown_verdict_t;

static teardown_verdict_t teardown_leak_attempt(void)
{
    if (!teardown_leak_wait_quiet()) {
        // Not a failure of teardown — a failure to get a quiet window, which
        // makes any number we produce meaningless. Say which it is: a test
        // that can't measure must not report a verdict it didn't earn.
        printd(DEBUG_TESTS, "\ttask_teardown_leak: undertaker never went quiet — "
                            "burials still completing after 20s\n");
        return TL_DIRTY;
    }
    if (!teardown_leak_wait_allocation_still()) {
        printd(DEBUG_TESTS, "\ttask_teardown_leak: the free-page count would not hold "
                            "still — something on this machine is allocating\n");
        return TL_DIRTY;
    }

    for (int i = 0; i < TEARDOWN_LEAK_WARMUP_CYCLES; i++) {
        if (!teardown_leak_one_cycle())
            return TL_ERROR;
    }

    uint64_t free_before = 0, free_after = 0;
    allocator_memory_snapshot(&free_before, NULL, NULL);
    uint64_t reclaimed_before = kTaskVmaReclaimedBytes;
    // WHOSE FUNERALS HAPPENED IN OUR WINDOW? Exactly one per cycle, or the
    // delta below belongs partly to somebody else and cannot be read as ours.
    //
    // Settling BEFORE the window is not enough, and that is not a guess: with
    // this test moved to the LATE phase it runs beside a live shell, and one
    // `ps` typed while it measured turned "a task costs nothing" into a
    // failure on the glass. The stillness it needs is stillness THROUGHOUT,
    // and the honest way to get it is not to wait longer — a user can always
    // out-wait you — but to notice afterwards that the window was not ours
    // alone, and decline to draw a conclusion from it.
    uint64_t burials_before = kTaskBurialCount;

    for (int i = 0; i < TEARDOWN_LEAK_MEASURED_CYCLES; i++) {
        if (!teardown_leak_one_cycle())
            return TL_ERROR;
    }

    uint64_t burials = kTaskBurialCount - burials_before;
    if (burials != (uint64_t)TEARDOWN_LEAK_MEASURED_CYCLES) {
        printd(DEBUG_TESTS, "\ttask_teardown_leak: %lu burials in a window that should have "
                            "held %d — this machine was busy\n",
               burials, TEARDOWN_LEAK_MEASURED_CYCLES);
        return TL_DIRTY;
    }

    // (A TEMP DIAG surviving-extent differ lived here during the
    // scribbled-text hunt, 2026-08-14/15. Its finest hour: it photographed
    // the pre-reclaim leak as two 4KB extents whose first bytes were
    // `55 48 89 e5...` — a function prologue, the fixture's own leaked text
    // page. Removed after the deferral was paid; the two clauses below are
    // the permanent instrument.)

    allocator_memory_snapshot(&free_after, NULL, NULL);
    uint64_t reclaimed = kTaskVmaReclaimedBytes - reclaimed_before;

    // Signed on purpose: free memory going UP across the window is not a leak,
    // it is the allocator having coalesced or some cache having shrunk, and
    // reading that as a colossal unsigned "loss" would be the classic
    // wraparound bug dressed as a test failure.
    int64_t lost = (int64_t)free_before - (int64_t)free_after;

    printd(DEBUG_TESTS,
           "\ttask_teardown_leak: %d cycles — free %lu -> %lu (lost %ld bytes), "
           "VMA backing reclaimed %lu bytes\n",
           TEARDOWN_LEAK_MEASURED_CYCLES, free_before, free_after, lost, reclaimed);

    // Clause 1: a cycle costs nothing. Since the deferral was paid
    // (2026-08-15) there is no legitimate remainder to subtract — any loss at
    // all is an undeclared leak.
    if (lost != 0) {
        printd(DEBUG_TESTS,
               "\ttask_teardown_leak: %ld bytes per %d cycles unaccounted for "
               "(%ld bytes/task)\n",
               lost, TEARDOWN_LEAK_MEASURED_CYCLES,
               lost / TEARDOWN_LEAK_MEASURED_CYCLES);
        return TL_ANOMALY;
    }

    // Clause 2: the reclaim provably RAN. A zero delta alone could mean the
    // fixture faulted nothing in (a demand-paging break passing vacuously);
    // glutton guarantees at least data+bss+heap resident per cycle, so the
    // undertaker must have freed at least that much.
    uint64_t reclaim_floor =
        GLUTTON_MIN_RECLAIM_PER_CYCLE * TEARDOWN_LEAK_MEASURED_CYCLES;
    if (reclaimed < reclaim_floor) {
        printd(DEBUG_TESTS,
               "\ttask_teardown_leak: reclaim ran short: %lu bytes freed over %d cycles, "
               "floor is %lu (glutton's data+bss+heap)\n",
               reclaimed, TEARDOWN_LEAK_MEASURED_CYCLES, reclaim_floor);
        return TL_ANOMALY;
    }

    printd(DEBUG_TESTS,
           "\tPASS: test_task_teardown_leak (a task costs nothing: 0 bytes lost, "
           "%lu bytes of VMA backing reclaimed over %d cycles)\n",
           reclaimed, TEARDOWN_LEAK_MEASURED_CYCLES);
    return TL_CLEAN;
}

// A LEAK IS SOMETHING THAT HAPPENS EVERY TIME. A single bad measurement is
// not, and on a live machine it usually is not even about us: the block cache
// takes 64KB at a time and never gives it back, so one `ls` typed while this
// runs drops the free count by a quarter of a megabyte that no burial will
// return. That is a cache doing its job, and calling it a leak on the glass
// is how a suite teaches people to ignore it.
//
// So: measure, and if the number is wrong, measure AGAIN on a fresh quiet
// window. Only a loss that survives every attempt is a defect — anything
// transient belonged to whatever else the machine was doing. The cost of the
// retries is why this test wants the LATE phase; nobody is waiting on it.
#define TEARDOWN_LEAK_ATTEMPTS 3

static bool test_task_teardown_leak(void)
{
    if (kRootFilesystem == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_task_teardown_leak (no root filesystem mounted)\n");
        return true;
    }

    // COUNT THE ATTEMPTS, do not just remember the last one. The verdict used
    // to read whatever `v` happened to hold when the loop ended, which is a
    // different question from the one being asked: DIRTY,DIRTY,ANOMALY
    // announced that "the loss REPEATED across 3 windows" on the strength of
    // ONE measurement, and ANOMALY,ANOMALY,DIRTY reported "never measurable"
    // after measuring twice. Both are the wrong story, and on a live machine
    // — where dirty windows are the expected case — both were reachable.
    // (Codex, PR #42.)
    int measured = 0;    // windows that produced a number: every one an anomaly,
                         // since a clean one returns immediately below
    int dirty = 0;       // windows that could not be measured at all

    for (int attempt = 1; attempt <= TEARDOWN_LEAK_ATTEMPTS; attempt++) {
        teardown_verdict_t v = teardown_leak_attempt();
        if (v == TL_CLEAN)
            return true;
        if (v == TL_ERROR) {
            printd(DEBUG_TESTS, "\tFAIL: test_task_teardown_leak - a cycle could not be run "
                                "(spawn or burial failed); nothing was measured\n");
            return false;
        }
        if (v == TL_ANOMALY)
            measured++;
        else
            dirty++;
        printd(DEBUG_TESTS, "\ttask_teardown_leak: attempt %d of %d was %s — retrying on a "
                            "fresh window\n",
               attempt, TEARDOWN_LEAK_ATTEMPTS, v == TL_DIRTY ? "unmeasurable" : "anomalous");
    }

    if (measured == 0) {
        // Never measurable. Not a verdict about teardown, and it must not
        // pretend to be one.
        printd(DEBUG_TESTS, "\tSKIP: test_task_teardown_leak (never got a window this "
                            "machine held still for — %d of %d attempts unmeasurable)\n",
               dirty, TEARDOWN_LEAK_ATTEMPTS);
        return true;
    }

    // ONE anomaly is not a leak; it is a measurement. The whole reason for
    // retrying is that a single bad window is usually the machine — a block
    // cache taking 64KB it will never give back is indistinguishable from a
    // leak inside one window and obvious across two. So a lone anomaly beside
    // windows we could not measure is INCONCLUSIVE, and says so, with the
    // number in it: a suite that cries wolf gets ignored, and so does one
    // that swallows a real finding in silence.
    if (measured < 2) {
        printd(DEBUG_TESTS,
               "\tSKIP: test_task_teardown_leak (1 window measured a loss and %d could not "
               "be measured at all — one measurement is not a repeat, so this is "
               "inconclusive rather than a leak. The number is above; a quiet machine "
               "settles it)\n", dirty);
        return true;
    }

    printd(DEBUG_TESTS,
           "\tFAIL: test_task_teardown_leak - the loss REPEATED across %d independently "
           "measured windows, so it is not the machine: since 2026-08-15 burial reclaims "
           "everything a task owned, and something allocated in task_create (or on the "
           "task's behalf) has no matching free in task_destroy. The per-attempt numbers "
           "are above.\n",
           measured);
    return false;
}


// ── The ext2 real-partition test ─────────────────────────────────────────────
// Two-way cross-implementation proof, one test:
//
//   READ half: content formatted by the HOST's mkfs.ext2 and populated by
//   debugfs (see the GNUmakefile disk rule) — os64 never wrote a byte of it.
//   Reading it back proves we parse real ext2, not our own private dialect;
//   self-written files can never give that proof, because a driver wrong the
//   same way in both directions reads itself back perfectly. These fixtures
//   are PROMISED on build-authored images; on a live writable root they may
//   have been tidied away, and that is the user USING the filesystem, not a
//   failure (2026-08-08, the P5 root cleanup) — so the fixture checks gate
//   on presence and skip loudly instead of failing.
//
//   WRITE half (same ruling: unless prior existence IS the point, a test
//   puts its own files there): the test authors /etc/testdata itself through
//   the mounted filesystem — a deep path and a pattern file sized from the
//   LIVE block size to climb the whole indirection ladder, which exercises
//   write-side block allocation through the single- and double-indirect
//   regimes on every boot. The files STAY on disk deliberately: the next
//   time Linux mounts this partition, e2fsck audits what os64 wrote. Linux
//   writes/os64 reads; os64 writes/Linux checks. 🤝
//
// pattern.bin's 16-byte records are self-describing ("00001234:os64e2\n" =
// record 1234 at byte 1234*16), so seeking into each mapping regime and
// asking the record its own name catches any off-by-one the block-map walk
// could commit. (Fixture-file region math is documented in
// tools/gen_ext2_testdata.py; the fixture image pins block size 1024 at
// mkfs time — one more reason the self-written file computes its own.)
#define EXT2_PATTERN_RECORDS 98304UL
#define EXT2_PATTERN_SIZE    (EXT2_PATTERN_RECORDS * 16)

// Failures speak on the GLASS as well as the log — same doctrine as MT_FAIL
// below (2026-08-08, the P5's first disk-root boot): a nolog bare-metal run
// swallows the printd reason, and a halting failure whose cause is invisible
// is a tripwire with a silencer. The macro emits both and returns false, so
// it serves ext2_check_record and the test body alike.
#define E2_FAIL(...) do { \
        printf("FAIL ext2_real: " __VA_ARGS__); \
        printd(DEBUG_TESTS, "\tFAIL: test_ext2_real_partition - " __VA_ARGS__); \
        return false; \
    } while (0)

static bool ext2_check_record(vfs_filesystem_t *fs, vfs_file_t *f, uint64_t offset, const char *region)
{
    char got[17], want[20];

    if (fs->fops->seek(f, (long)offset, SEEK_SET) < 0)
        E2_FAIL("seek to %lu (%s region) failed\n", offset, region);
    if (fs->fops->read(f, got, 16) != 16)
        E2_FAIL("read at %lu (%s region) short\n", offset, region);
    got[16] = '\0';
    sprintf(want, "%08lu:os64e2\n", (unsigned long)(offset / 16));
    if (strncmp(got, want, 16) != 0)
        E2_FAIL("%s region record at %lu: got '%s', want '%s'\n",
                region, offset, got, want);
    return true;
}

static bool test_ext2_real_partition(void)
{
    // Find the ext2 partition by detected filesystem type — the boot flow's
    // superblock probe (filesystem.c) marks it during storage init.
    block_device_info_t *edev = NULL;
    int epart = -1;
    for (int d = 0; d < kBlockDeviceInfoCount && edev == NULL; d++)
    {
        block_device_info_t *dev = &kBlockDeviceInfo[d];
        if (dev->block_device == NULL || dev->block_device->partition_table == NULL)
            continue;
        for (int p = 0; p < dev->block_device->partition_table->partCount; p++)
        {
            if (dev->block_device->partition_table->parts[p]->filesystemType != FILESYSTEM_TYPE_EXT2)
                continue;
            edev = dev;
            epart = p;
            break;
        }
    }
    if (edev == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_ext2_real_partition (no ext2 partition detected)\n");
        return true;
    }

    // ── Phase 1: self-provision /etc/testdata, through the MOUNTED
    // filesystem. Writes must go through the live mount and nothing else:
    // the write driver keeps allocation state (bitmaps, group counts) in its
    // one instance, and a second write instance on the same partition would
    // fight it over that state. The read-back below gets its own instance
    // precisely because reading is the safe half.
    vfs_filesystem_t *mfs = NULL;
    for (int m = 0; m < kMountCount; m++)
        if (kMountTable[m].fs != NULL &&
            kMountTable[m].fs->block_device_info == edev &&
            kMountTable[m].fs->partNumber == epart)
        {
            mfs = kMountTable[m].fs;
            break;
        }

    vfs_file_t *f = NULL;
    uint64_t bs = 0, single_at = 0, double_at = 0, self_size = 0;
    bool self_written = false;
    static const char self_deep[] = "written by os64, verified by os64, audited by e2fsck\n";
    if (mfs != NULL && mfs->fops->write != NULL)
    {
        // mkdir returns -1 for "already exists" and "failed" alike, so the
        // chain is verified by opening the leaf afterward rather than by
        // trusting the returns. ("w"-mode creation below is idempotent the
        // same way — reruns truncate and rewrite, exercising that path free.)
        static const char *dirs[] = { "/etc", "/etc/testdata",
                                      "/etc/testdata/dir1", "/etc/testdata/dir1/dir2" };
        char pathbuf[40];
        for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
            sprintf(pathbuf, "%s", dirs[i]);   // mkdir wants a mutable path
            mfs->dops->mkdir(pathbuf, mfs);
        }
        vfs_directory_t *dchk = NULL;
        if (mfs->dops->open(&dchk, "/etc/testdata/dir1/dir2", mfs) != 0)
            E2_FAIL("self-provision mkdir chain failed\n");
        mfs->dops->close(dchk);

        if (mfs->fops->open(&f, "/etc/testdata/dir1/dir2/deep.txt", "w", mfs) != 0)
            E2_FAIL("self-provision create deep.txt failed\n");
        int wn = mfs->fops->write(f, self_deep, sizeof(self_deep) - 1);
        mfs->fops->close(f);
        if (wn != (int)(sizeof(self_deep) - 1))
            E2_FAIL("self-provision write deep.txt short (%d)\n", wn);

        // Pattern-file geometry from the LIVE block size (the fixture image
        // pins 1024, but a big root is 4096 and every boundary moves): 12
        // direct pointers, then a single-indirect block of bs/4 pointers,
        // then the double-indirect tree. Ending 4 blocks past the double-
        // indirect boundary climbs the whole ladder — and keeps the total a
        // multiple of the 4096-byte chunk for every legal bs (1024/2048/4096).
        bs = (uint64_t)mfs->blockSize;
        single_at = 12 * bs;
        double_at = single_at + (bs / 4) * bs;
        self_size = double_at + 4 * bs;
        if (mfs->fops->open(&f, "/etc/testdata/pattern.bin", "w", mfs) != 0)
            E2_FAIL("self-provision create pattern.bin failed\n");
        char chunk[4096 + 1];   // +1: sprintf lands a NUL one past each record
        for (uint64_t off = 0; off < self_size; off += 4096)
        {
            for (unsigned rec = 0; rec < 4096 / 16; rec++)
                sprintf(chunk + rec * 16, "%08lu:os64e2\n", (unsigned long)(off / 16 + rec));
            wn = mfs->fops->write(f, chunk, 4096);
            if (wn != 4096) {
                mfs->fops->close(f);
                E2_FAIL("self-provision write pattern.bin short at %lu (%d)\n", off, wn);
            }
        }
        mfs->fops->close(f);
        self_written = true;
        // The files STAY on disk on purpose: the next time Linux mounts
        // this partition, e2fsck audits what os64 wrote — the reverse half
        // of the cross-implementation handshake Phase 3 runs forward.
    }
    else
    {
        printf("note ext2_real: mount is read-only, self-provision skipped\n");
        printd(DEBUG_TESTS, "\tNOTE: test_ext2_real_partition - read-only mount, self-provision skipped\n");
    }

    // ── Phase 2: an independent READ-ONLY driver instance, assembled by
    // hand and deliberately BYPASSING the mount table: this is the
    // driver-level test (test_mount_table covers the routed path), and
    // reading Phase 1's files back through a second instance proves the
    // bytes reached the media, not just the writer's own state. Built AFTER
    // the writes so its superblock/group snapshot is current. Read-only, no
    // locks — harmless beside the live mount precisely because it never writes.
    vfs_filesystem_t *fs = kmalloc(sizeof(vfs_filesystem_t));
    if (fs == NULL)
        E2_FAIL("kmalloc of test fs object failed\n");
    memset(fs, 0, sizeof(vfs_filesystem_t));
    fs->partNumber = epart;
    fs->block_device_info = edev;
    fs->bops = edev->block_device->ops;
    fs->fops = &ext2_fops;
    fs->dops = &ext2_dops;
    if (ext2_initialize_filesystem(fs) != 0)
        E2_FAIL("superblock/groups mount failed\n");

    // ── Phase 3: content authored by LINUX, read by os64 — each fixture
    // gates on ITS OWN presence (2026-08-08 lesson #2, same morning as
    // lesson #1: a human tidies with human granularity. The P5 cleanup took
    // dir1 but spared hello.txt, and an all-or-nothing gate keyed on
    // hello.txt marched straight into the missing directory). Absent = the
    // user used the filesystem — note it, move on. PRESENT but wrong = the
    // driver misreading a Linux-authored file = FAIL.
    char buf[128];
    int n;
    bool fx_hello = false, fx_deep = false, fx_pattern = false;
    if (fs->fops->open(&f, "/hello.txt", "r", fs) == 0)
    {
        fx_hello = true;
        // 1. A small file, byte-for-byte.
        static const char hello_expect[] =
            "Hello from a real ext2 filesystem — written by Linux, read by os64!\n";
        n = fs->fops->read(f, buf, sizeof(buf));
        fs->fops->close(f);
        if (n != (int)(sizeof(hello_expect) - 1) || strncmp(buf, hello_expect, sizeof(hello_expect) - 1) != 0)
            E2_FAIL("/hello.txt content mismatch (n=%d)\n", n);
    }
    if (fs->fops->open(&f, "/dir1/dir2/deep.txt", "r", fs) == 0)
    {
        fx_deep = true;
        // 2. Path resolution three directories deep.
        n = fs->fops->read(f, buf, sizeof(buf));
        fs->fops->close(f);
        if (n <= 0 || strncmp(buf, "the deep file", 13) != 0)
            E2_FAIL("deep.txt content mismatch\n");
    }
    if (fs->fops->open(&f, "/pattern.bin", "r", fs) == 0)
    {
        fx_pattern = true;
        // 3. The block-map workout at the FIXTURE's pinned 1KB geometry
        //    (region math: tools/gen_ext2_testdata.py).
        fs->fops->seek(f, 0, SEEK_END);
        if ((uint64_t)fs->fops->tell(f) != EXT2_PATTERN_SIZE) {
            int at = fs->fops->tell(f);
            fs->fops->close(f);
            E2_FAIL("pattern.bin size %d, want %lu\n", at, EXT2_PATTERN_SIZE);
        }
        bool ok = ext2_check_record(fs, f, 0,                      "first")
               && ext2_check_record(fs, f, 4096,                   "direct")
               && ext2_check_record(fs, f, 100000,                 "single-indirect")
               && ext2_check_record(fs, f, 1000000,                "double-indirect")
               && ext2_check_record(fs, f, EXT2_PATTERN_SIZE - 16, "last");
        fs->fops->close(f);
        if (!ok)
            return false;
    }
    if (!fx_hello || !fx_deep || !fx_pattern)
    {
        printf("note ext2_real: absent mkfs fixtures skipped (hello=%d deep=%d pattern=%d)\n",
               fx_hello, fx_deep, fx_pattern);
        printd(DEBUG_TESTS, "\tNOTE: test_ext2_real_partition - absent mkfs fixtures skipped (hello=%d deep=%d pattern=%d)\n",
               fx_hello, fx_deep, fx_pattern);
    }

    // ── Phase 4: read back what Phase 1 wrote, through the independent
    // instance, at the live geometry's regime boundaries.
    if (self_written)
    {
        if (fs->fops->open(&f, "/etc/testdata/dir1/dir2/deep.txt", "r", fs) != 0)
            E2_FAIL("read-back open self deep.txt failed\n");
        n = fs->fops->read(f, buf, sizeof(buf));
        fs->fops->close(f);
        if (n != (int)(sizeof(self_deep) - 1) || strncmp(buf, self_deep, sizeof(self_deep) - 1) != 0)
            E2_FAIL("self deep.txt read-back mismatch (n=%d)\n", n);

        if (fs->fops->open(&f, "/etc/testdata/pattern.bin", "r", fs) != 0)
            E2_FAIL("read-back open self pattern.bin failed\n");
        fs->fops->seek(f, 0, SEEK_END);
        if ((uint64_t)fs->fops->tell(f) != self_size) {
            int at = fs->fops->tell(f);
            fs->fops->close(f);
            E2_FAIL("self pattern.bin size %d, want %lu\n", at, self_size);
        }
        bool ok = ext2_check_record(fs, f, 0,              "first")
               && ext2_check_record(fs, f, bs,             "direct")
               && ext2_check_record(fs, f, single_at,      "single-indirect")
               && ext2_check_record(fs, f, double_at,      "double-indirect")
               && ext2_check_record(fs, f, self_size - 16, "last");
        fs->fops->close(f);
        if (!ok)
            return false;
    }

    // ── Phase 5: the root listing through the fs-neutral dirent seam — same
    // contract ls uses, different filesystem, zero caller changes. It must
    // COMPLETE without error on ANY disk; specific names are only asserted
    // where the phases above found or created them.
    vfs_directory_t *dir = NULL;
    if (fs->dops->open(&dir, "/", fs) != 0)
        E2_FAIL("opendir / failed\n");
    os64_dirent_t de;
    bool saw_hello = false, saw_etc = false;
    int r;
    while ((r = fs->dops->read(dir, &de)) == 1)
    {
        if (strncmp(de.name, "hello.txt", 10) == 0 && !(de.flags & OS64_DE_DIR))
            saw_hello = true;
        if (strncmp(de.name, "etc", 4) == 0 && (de.flags & OS64_DE_DIR))
            saw_etc = true;
    }
    fs->dops->close(dir);
    if (r != 0)
        E2_FAIL("root readdir error (%d)\n", r);
    if (fx_hello && !saw_hello)
        E2_FAIL("root listing missing hello.txt though it opened\n");
    if (self_written && !saw_etc)
        E2_FAIL("root listing missing /etc though Phase 1 just made it\n");

    // ── Phase 6: absence must fail in-band, like everything else in this kernel.
    if (fs->fops->open(&f, "/no/such/file", "r", fs) == 0)
        E2_FAIL("bogus path opened\n");

    printd(DEBUG_TESTS, "\tPASS: test_ext2_real_partition (self-provision=%s [bs=%lu], "
           "mkfs-fixtures=%d/3, root listing, bogus path)\n",
           self_written ? "written+verified" : "skipped", bs,
           fx_hello + fx_deep + fx_pattern);
    return true;
}
#undef E2_FAIL

// The mount table: longest-prefix routing over the LIVE table built at boot.
// Semantics first (boundaries, tails), then real I/O through whatever
// secondary mounts this boot actually produced — "/ext2" when FAT is root,
// "/fat" when ext2 is (both partitions carry known content, so either way
// there is something to verify end to end).
//
// Failures speak on the GLASS as well as the log (2026-08-08): this test
// failed on the P5's first disk-root boot, the entry's nolog swallowed the
// printd reason, and a halting failure whose cause is invisible on bare
// metal is a tripwire with a silencer. The macro emits both.
#define MT_FAIL(...) do { \
        printf("FAIL mount_table: " __VA_ARGS__); \
        printd(DEBUG_TESTS, "\tFAIL: test_mount_table - " __VA_ARGS__); \
        return false; \
    } while (0)

static bool test_mount_table(void)
{
    if (kMountCount < 1 || kRootFilesystem == NULL)
        MT_FAIL("no mounts (count=%d)\n", kMountCount);

    // 1. The root always resolves, tail unchanged.
    const char *tail = NULL;
    vfs_filesystem_t *fs = vfs_resolve_mount("/", &tail);
    if (fs != kRootFilesystem || strncmp(tail, "/", 2) != 0)
        MT_FAIL("'/' did not resolve to root\n");

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
        if (fs != m->fs || strncmp(tail, "/", 2) != 0)
            MT_FAIL("'%s' exact match wrong (tail=%s)\n",
                    m->prefix, tail ? tail : "NULL");

        sprintf(probe, "%s/x", m->prefix);
        fs = vfs_resolve_mount(probe, &tail);
        if (fs != m->fs || strncmp(tail, "/x", 3) != 0)
            MT_FAIL("'%s' child tail wrong (tail=%s)\n",
                    probe, tail ? tail : "NULL");

        sprintf(probe, "%szz", m->prefix);
        fs = vfs_resolve_mount(probe, &tail);
        if (fs == m->fs) {
            MT_FAIL("'%s' matched prefix '%s' (boundary leak)\n",
                    probe, m->prefix);
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
            fs->dops->open(&dir, tail, fs) != 0)
            MT_FAIL("opendir %s failed\n", m->prefix);
        os64_dirent_t de;
        int entries = 0;
        int rc;
        while ((rc = fs->dops->read(dir, &de)) == 1)
            entries++;
        fs->dops->close(dir);
        if (rc < 0)
            MT_FAIL("%s readdir error (%d)\n", m->prefix, rc);
        // An EMPTY listing is not a failure: the P5's first disk-root boot
        // (2026-08-08) brought a freshly-mkfs'd /home that had never held a
        // file, and readdir never delivers "." / ".." — zero entries is
        // exactly what a healthy newborn mount looks like. Emptiness is
        // only suspicious where content is PROMISED, and the /ext2 and
        // /fat probes below enforce precisely that.
        printd(DEBUG_TESTS | DEBUG_DETAILED, "\tmount_table: %s listed %d entries\n",
               m->prefix, entries);

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
                fs->fops->open(&f, tail, "r", fs) != 0)
                MT_FAIL("open %s failed\n", file_probe);
            int n = fs->fops->read(f, buf, sizeof(buf));
            fs->fops->close(f);
            if (n <= 0 || (expect != NULL && strncmp(buf, expect, strlen(expect)) != 0)) {
                MT_FAIL("%s content wrong (n=%d)\n", file_probe, n);
            }
            verified++;
        }
    }

    printd(DEBUG_TESTS, "\tPASS: test_mount_table (%d mounts, %d routed content checks)\n",
           kMountCount, verified);
    return true;
}
#undef MT_FAIL

// ── /dev — the devices answer for themselves (2026-08-20) ────────────────────
// The interactive half of this is easy (`cat /dev/null`, `cp x /dev/full`) and
// was done by hand the day devfs landed. This test exists for the half a shell
// CANNOT do: /dev/zero is an ENDLESS faucet, so there is no husk incantation
// that reads a bounded piece of it — `cat` would run until the heat death, and
// `head -n` waits for a newline that a stream of NULs never contains.
//
// Everything here goes through the VFS the way a syscall would (resolve the
// mount, dispatch through the fs's own op tables), so the routing is under
// test alongside the devices themselves.
#define DEV_FAIL(...) do { \
        printf("FAIL devfs: " __VA_ARGS__); \
        printd(DEBUG_TESTS, "\tFAIL: test_devfs - " __VA_ARGS__); \
        return false; \
    } while (0)

// Open one /dev node through the mount table, exactly as syscall_open would.
static vfs_file_t *devtest_open(const char *path, const char *mode)
{
    const char *tail = NULL;
    vfs_filesystem_t *fs = vfs_resolve_mount(path, &tail);
    vfs_file_t *f = NULL;

    if (fs == NULL || fs->fops == NULL || fs->fops->open == NULL)
        return NULL;
    if (fs->fops->open(&f, tail, mode, fs) != 0)
        return NULL;
    return f;
}

static bool test_devfs(void)
{
    const char *tail = NULL;
    vfs_filesystem_t *devfs = vfs_resolve_mount("/dev", &tail);
    unsigned char buf[64];

    if (devfs == NULL || strncmp(tail, "/", 2) != 0)
        DEV_FAIL("/dev did not resolve to a mount\n");
    if (devfs == kRootFilesystem)
        DEV_FAIL("/dev resolved to the ROOT fs — the mount never claimed its prefix\n");

    // ── /dev/null: empty on read, bottomless on write ───────────────────────
    vfs_file_t *f = devtest_open("/dev/null", "r");
    if (f == NULL)
        DEV_FAIL("open /dev/null failed\n");
    memset(buf, 0xAA, sizeof(buf));
    int got = f->fops->read(f, buf, sizeof(buf));
    if (got != 0)
        DEV_FAIL("/dev/null read returned %d, expected 0 (EOF)\n", got);
    // The buffer must be UNTOUCHED: a read that returns 0 has written nothing,
    // and a caller that trusts the count would otherwise get 64 bytes of a
    // lie it never asked for.
    for (size_t i = 0; i < sizeof(buf); i++)
        if (buf[i] != 0xAA)
            DEV_FAIL("/dev/null read scribbled on the buffer at %u\n", (unsigned)i);
    f->fops->close(f);

    f = devtest_open("/dev/null", "w");
    if (f == NULL)
        DEV_FAIL("open /dev/null for write failed\n");
    got = f->fops->write(f, "swallow this", 12);
    if (got != 12)
        DEV_FAIL("/dev/null write returned %d, expected 12 (whole count)\n", got);
    f->fops->close(f);

    // ── /dev/zero: the faucet, and the reason this test exists ──────────────
    f = devtest_open("/dev/zero", "r");
    if (f == NULL)
        DEV_FAIL("open /dev/zero failed\n");
    for (int round = 0; round < 3; round++)
    {
        memset(buf, 0xAA, sizeof(buf));
        got = f->fops->read(f, buf, sizeof(buf));
        if (got != (int)sizeof(buf))
            DEV_FAIL("/dev/zero read %d returned %d, expected %u — a faucet "
                     "never runs short and never reaches EOF\n",
                     round, got, (unsigned)sizeof(buf));
        for (size_t i = 0; i < sizeof(buf); i++)
            if (buf[i] != 0x00)
                DEV_FAIL("/dev/zero delivered 0x%02x at offset %u\n",
                         buf[i], (unsigned)i);
    }
    // Reading zero bytes is not EOF, it is a request for nothing — and it must
    // not be confused with the end of a stream that has no end.
    got = f->fops->read(f, buf, 0);
    if (got != 0)
        DEV_FAIL("/dev/zero zero-length read returned %d, expected 0\n", got);
    f->fops->close(f);

    // ── /dev/full: reads like zero, refuses every write ─────────────────────
    f = devtest_open("/dev/full", "r");
    if (f == NULL)
        DEV_FAIL("open /dev/full failed\n");
    memset(buf, 0xAA, sizeof(buf));
    got = f->fops->read(f, buf, sizeof(buf));
    if (got != (int)sizeof(buf) || buf[0] != 0 || buf[sizeof(buf) - 1] != 0)
        DEV_FAIL("/dev/full read returned %d — it reads as zeros, like Linux's\n", got);
    f->fops->close(f);

    f = devtest_open("/dev/full", "w");
    if (f == NULL)
        DEV_FAIL("open /dev/full for write failed — the OPEN succeeds, the "
                 "WRITE is what fails\n");
    got = f->fops->write(f, "no room", 7);
    if (got >= 0)
        DEV_FAIL("/dev/full write returned %d — os64's one write that must "
                 "always fail just succeeded\n", got);
    f->fops->close(f);

    // ── Paths that must NOT exist ───────────────────────────────────────────
    // A permissive parser is how "/dev/nul" quietly becomes the void and a
    // typo'd redirect eats output forever.
    if (devtest_open("/dev/nul", "r") != NULL)
        DEV_FAIL("/dev/nul opened — the name match is not exact\n");
    if (devtest_open("/dev/null/x", "r") != NULL)
        DEV_FAIL("/dev/null/x opened — a trailing component was ignored\n");
    if (devtest_open("/dev/tty", "r") != NULL)
        DEV_FAIL("/dev/tty opened as a FILE — it is a handle alias answered by "
                 "syscall_open, and this layer must refuse it (devfs.h)\n");

    // ── The listing names every resident ────────────────────────────────────
    // A directory that drops a name reads exactly like a name that does not
    // exist (sysfs learned this on 2026-08-20, five days of "net is missing").
    vfs_directory_t *dir = NULL;
    if (devfs->dops == NULL || devfs->dops->open == NULL ||
        devfs->dops->open(&dir, "/", devfs) != 0)
        DEV_FAIL("opendir /dev failed\n");
    bool saw_null = false, saw_zero = false, saw_full = false, saw_tty = false;
    os64_dirent_t de;
    int rc, entries = 0;
    while ((rc = devfs->dops->read(dir, &de)) == 1)
    {
        entries++;
        if (strcmp(de.name, "null") == 0) saw_null = true;
        else if (strcmp(de.name, "zero") == 0) saw_zero = true;
        else if (strcmp(de.name, "full") == 0) saw_full = true;
        else if (strcmp(de.name, "tty")  == 0) saw_tty  = true;
    }
    devfs->dops->close(dir);
    if (rc < 0)
        DEV_FAIL("/dev readdir error (%d)\n", rc);
    if (!saw_null || !saw_zero || !saw_full || !saw_tty)
        DEV_FAIL("/dev listing missing a resident (null=%d zero=%d full=%d tty=%d)\n",
                 saw_null, saw_zero, saw_full, saw_tty);
    printd(DEBUG_TESTS | DEBUG_DETAILED, "\tdevfs: /dev listed %d entries\n", entries);

    // stat answers for the directory itself and for one resident.
    if (devfs->dops->stat == NULL)
        DEV_FAIL("/dev has no stat\n");
    memset(&de, 0, sizeof(de));
    if (devfs->dops->stat("/", &de, devfs) != 0 || !(de.flags & OS64_DE_DIR))
        DEV_FAIL("stat /dev did not report a directory\n");
    memset(&de, 0, sizeof(de));
    if (devfs->dops->stat("/zero", &de, devfs) != 0 || strcmp(de.name, "zero") != 0)
        DEV_FAIL("stat /dev/zero failed\n");
    memset(&de, 0, sizeof(de));
    if (devfs->dops->stat("/nope", &de, devfs) == 0)
        DEV_FAIL("stat /dev/nope succeeded — a name that does not exist\n");

    return true;
}
#undef DEV_FAIL


// The root-filesystem WRITE path, properly inside the framework at last:
// create/write/read-back a file, mkdir, create/write/read-back inside the new
// directory. This is the useful half of the old boot-time testVFS() — which
// lived OUTSIDE the framework in tests.c, ran from kernel.c after the suite,
// and PANICKED on failure (its panic ate a Friday: "Root filesystem disk test
// failed: 4294967291" was this code trusting a then-void nvme write). Ported
// at Chris's call, 2026-07-19; the legacy block and tests.c are gone. The
// read half wasn't ported — file_io/dir_list/mount_table already cover reads
// through the real syscall path.
//
// Groomed 2026-08-08 (the pre-"us" nvme-era draft finally tidied, at Chris's
// call): write returns are CHECKED now (ignoring them was the exact bug that
// ate that Friday), failures speak on the glass, and the litter moved off
// the root — the old version planted /test2 and /testdir at "/" every boot,
// which on a now-writable ext2 root is the test suite littering the very
// disk the user keeps clean. Everything lives under /etc/testdata, per the
// same-day self-provisioning ruling.
#define VW_FAIL(...) do { \
        printf("FAIL vfs_write_mkdir: " __VA_ARGS__); \
        printd(DEBUG_TESTS, "\tFAIL: test_vfs_write_mkdir - " __VA_ARGS__); \
        return false; \
    } while (0)


static bool test_vfs_write_mkdir(void)
{
    if (kRootFilesystem == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_vfs_write_mkdir (no root filesystem mounted)\n");
        return true;
    }
    // Writing needs a filesystem that writes AND a device that writes. Since
    // the 2026-08-07 ratification every ext2 mount is read-write, so this
    // runs on BOTH root flavors now; the gate survives for forced_ro mounts
    // and any future read-only filesystem.
    if (kRootFilesystem->fops->write == NULL || kRootFilesystem->bops->write == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_vfs_write_mkdir (root filesystem is read-only)\n");
        return true;
    }

    static const char msg1[] = "Hello world from Chris!\n";        // heritage strings —
    static const char msg2[] = "Hello world from Chris too!\n";    // testVFS's originals
    char buf[64];
    vfs_file_t *f = NULL;
    int n;

    // 0. The tidy corner. mkdir speaks 0/-1 with -1 covering both "exists"
    //    and "failed" (the seam went fs-neutral 2026-08-04 when ext2 became
    //    its second implementation and fat_mkdir's FRESULT leak was
    //    flattened) — so each mkdir is verified by stat, which tells a
    //    pre-existing directory apart from a real failure.
    static const char *dirs[] = { "/etc", "/etc/testdata", "/etc/testdata/testdir" };
    os64_dirent_t de;
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        char pathbuf[32];
        sprintf(pathbuf, "%s", dirs[i]);   // mkdir wants a mutable path
        kRootFilesystem->dops->mkdir(pathbuf, kRootFilesystem);
        if (kRootFilesystem->dops->stat(dirs[i], &de, kRootFilesystem) != 0
            || !(de.flags & OS64_DE_DIR))
            VW_FAIL("mkdir %s failed and it doesn't already exist\n", dirs[i]);
    }

    // 1. Create, write, read back.
    if (kRootFilesystem->fops->open(&f, "/etc/testdata/test2", "c", kRootFilesystem) != 0)
        VW_FAIL("create /etc/testdata/test2 failed\n");
    n = kRootFilesystem->fops->write(f, msg1, sizeof(msg1) - 1);
    kRootFilesystem->fops->close(f);
    f = NULL;
    if (n != (int)(sizeof(msg1) - 1))
        VW_FAIL("write to test2 short (%d)\n", n);
    if (kRootFilesystem->fops->open(&f, "/etc/testdata/test2", "r", kRootFilesystem) != 0)
        VW_FAIL("reopen /etc/testdata/test2 failed\n");
    n = kRootFilesystem->fops->read(f, buf, sizeof(buf));
    kRootFilesystem->fops->close(f);
    f = NULL;
    if (n != (int)(sizeof(msg1) - 1) || strncmp(buf, msg1, sizeof(msg1) - 1) != 0)
        VW_FAIL("test2 read-back mismatch (n=%d)\n", n);

    // 2. A file INSIDE the fresh directory — proves the mkdir made a real
    //    directory, not just a listing entry.
    if (kRootFilesystem->fops->open(&f, "/etc/testdata/testdir/testfile", "c", kRootFilesystem) != 0)
        VW_FAIL("create /etc/testdata/testdir/testfile failed\n");
    n = kRootFilesystem->fops->write(f, msg2, sizeof(msg2) - 1);
    kRootFilesystem->fops->close(f);
    f = NULL;
    if (n != (int)(sizeof(msg2) - 1))
        VW_FAIL("write to testdir/testfile short (%d)\n", n);
    if (kRootFilesystem->fops->open(&f, "/etc/testdata/testdir/testfile", "r", kRootFilesystem) != 0)
        VW_FAIL("reopen /etc/testdata/testdir/testfile failed\n");
    n = kRootFilesystem->fops->read(f, buf, sizeof(buf));
    kRootFilesystem->fops->close(f);
    if (n != (int)(sizeof(msg2) - 1) || strncmp(buf, msg2, sizeof(msg2) - 1) != 0)
        VW_FAIL("testdir/testfile read-back mismatch (n=%d)\n", n);

    printd(DEBUG_TESTS, "\tPASS: test_vfs_write_mkdir (create/write/read-back, mkdir, nested file — under /etc/testdata)\n");
    return true;
}
#undef VW_FAIL

// ── test_vfs_rename ─────────────────────────────────────────────────────────
// rename's proving ground (2026-08-16, the day the verb was built — os64get
// demanded it, per the consumer-driven rule). Runs against the ROOT
// filesystem on both root flavors, littering only under /etc/testdata and
// cleaning up after itself, per the self-provisioning ruling.
//
// What it actually proves, in order of how much it would hurt to get wrong:
//
//   1. ATOMIC REPLACE (the 2026-08-16 ruling). Renaming onto an existing
//      file leaves the DESTINATION NAME holding the SOURCE'S BYTES. This is
//      the assertion the whole feature exists for — if it ever fails, the
//      write-a-temp-then-publish recipe is a lie and os64get can eat a good
//      file.
//   2. A directory MOVE keeps its contents reachable through the new path.
//      That single read proves both halves of the surgery: the new parent's
//      entry AND the moved directory's rewritten "..".
//   3. Every refusal refuses AND LEAVES THE SOURCE INTACT. A refusal that
//      half-happened is worse than no refusal at all.
//
// The ext2-only steps are gated on the op table rather than a filesystem-type
// field (there isn't one): a mount whose rename IS ext2's is ext2's. FAT gets
// the shared assertions and is spared the two it cannot answer — the
// open-handle refusal (FAT keeps no open-inode count) and the loop refusal
// (which on FAT would mean deliberately asking the lifeboat to corrupt
// itself to see whether it declines).
#define RN_FAIL(...) do { \
        printf("FAIL vfs_rename: " __VA_ARGS__); \
        printd(DEBUG_TESTS, "\tFAIL: test_vfs_rename - " __VA_ARGS__); \
        return false; \
    } while (0)

// Read a whole small file back; returns the byte count, or -1 if it wouldn't
// even open. (A local helper because this test reads back nine times and the
// open/read/close triple is noise at that density.)
static int rn_slurp(const char *path, char *buf, int cap)
{
    vfs_file_t *f = NULL;
    if (kRootFilesystem->fops->open(&f, path, "r", kRootFilesystem) != 0)
        return -1;
    int n = kRootFilesystem->fops->read(f, buf, (size_t)cap);
    kRootFilesystem->fops->close(f);
    return n;
}

// Create a file holding exactly `text`. Returns true on success.
static bool rn_plant(const char *path, const char *text, int len)
{
    vfs_file_t *f = NULL;
    if (kRootFilesystem->fops->open(&f, path, "c", kRootFilesystem) != 0)
        return false;
    int n = kRootFilesystem->fops->write(f, text, (size_t)len);
    kRootFilesystem->fops->close(f);
    return n == len;
}

static bool test_vfs_rename(void)
{
    if (kRootFilesystem == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_vfs_rename (no root filesystem mounted)\n");
        return true;
    }
    if (kRootFilesystem->fops->write == NULL || kRootFilesystem->bops->write == NULL ||
        kRootFilesystem->fops->rename == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_vfs_rename (root filesystem is read-only or has no rename)\n");
        return true;
    }

    // A mount whose rename IS ext2's rename is an ext2 mount. See the header
    // comment for why this stands in for a filesystem-type field.
    bool is_ext2 = (kRootFilesystem->fops->rename == ext2_rw_fops.rename);

    static const char alpha[] = "ALPHA";   // 5 bytes — the bytes that must survive
    static const char omega[] = "OMEGA";   // 5 bytes — the bytes that must be replaced
    char buf[32];
    os64_dirent_t de;
    int n;

    // 0. Provision. mkdir speaks 0/-1 with -1 covering "already exists", so
    //    stat is the judge — same discipline as test_vfs_write_mkdir.
    static const char *dirs[] = { "/etc", "/etc/testdata",
                                  "/etc/testdata/rn", "/etc/testdata/rn2" };
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        char pathbuf[40];
        sprintf(pathbuf, "%s", dirs[i]);   // mkdir wants a mutable path
        kRootFilesystem->dops->mkdir(pathbuf, kRootFilesystem);
        if (kRootFilesystem->dops->stat(dirs[i], &de, kRootFilesystem) != 0
            || !(de.flags & OS64_DE_DIR))
            RN_FAIL("provision: %s is not a directory\n", dirs[i]);
    }
    // Start from a known floor — a previous boot's leftovers would make
    // "already exists" failures read as rename bugs.
    kRootFilesystem->fops->rm("/etc/testdata/rn/a", kRootFilesystem);
    kRootFilesystem->fops->rm("/etc/testdata/rn/b", kRootFilesystem);
    kRootFilesystem->fops->rm("/etc/testdata/rn/c", kRootFilesystem);
    kRootFilesystem->fops->rm("/etc/testdata/rn/f", kRootFilesystem);
    kRootFilesystem->fops->rm("/etc/testdata/rn2/c", kRootFilesystem);

    // 1. The plain rename, one directory. Old name gone, new name holds the
    //    bytes.
    if (!rn_plant("/etc/testdata/rn/a", alpha, sizeof(alpha) - 1))
        RN_FAIL("could not plant rn/a\n");
    if (kRootFilesystem->fops->rename("/etc/testdata/rn/a", "/etc/testdata/rn/b",
                                      kRootFilesystem) != 0)
        RN_FAIL("plain rename a -> b failed\n");
    if (kRootFilesystem->dops->stat("/etc/testdata/rn/a", &de, kRootFilesystem) == 0)
        RN_FAIL("old name rn/a still resolves after rename\n");
    n = rn_slurp("/etc/testdata/rn/b", buf, sizeof(buf));
    if (n != (int)(sizeof(alpha) - 1) || memcmp(buf, alpha, sizeof(alpha) - 1) != 0)
        RN_FAIL("rn/b read-back mismatch after rename (n=%d)\n", n);

    // 2. THE RULING: atomic replace. rn/c exists and holds OMEGA; renaming
    //    b onto it must leave c holding ALPHA — the source's bytes under the
    //    destination's name, with no third state in between.
    if (!rn_plant("/etc/testdata/rn/c", omega, sizeof(omega) - 1))
        RN_FAIL("could not plant rn/c\n");
    if (kRootFilesystem->fops->rename("/etc/testdata/rn/b", "/etc/testdata/rn/c",
                                      kRootFilesystem) != 0)
        RN_FAIL("replacing rename b -> c failed (the ruling says it must succeed)\n");
    if (kRootFilesystem->dops->stat("/etc/testdata/rn/b", &de, kRootFilesystem) == 0)
        RN_FAIL("source rn/b survived a replacing rename\n");
    n = rn_slurp("/etc/testdata/rn/c", buf, sizeof(buf));
    if (n != (int)(sizeof(alpha) - 1) || memcmp(buf, alpha, sizeof(alpha) - 1) != 0)
        RN_FAIL("replace left the WRONG bytes under rn/c (n=%d) — atomic replace is broken\n", n);

    // 3. Move to a different directory, same filesystem.
    if (kRootFilesystem->fops->rename("/etc/testdata/rn/c", "/etc/testdata/rn2/c",
                                      kRootFilesystem) != 0)
        RN_FAIL("cross-directory rename failed\n");
    n = rn_slurp("/etc/testdata/rn2/c", buf, sizeof(buf));
    if (n != (int)(sizeof(alpha) - 1) || memcmp(buf, alpha, sizeof(alpha) - 1) != 0)
        RN_FAIL("rn2/c read-back mismatch after move (n=%d)\n", n);

    // 4. A source that isn't there is a refusal, not a silent success.
    if (kRootFilesystem->fops->rename("/etc/testdata/rn/nothing_here",
                                      "/etc/testdata/rn/x", kRootFilesystem) == 0)
        RN_FAIL("rename of a nonexistent source reported success\n");

    // 5. A directory is never replaced — and the source must survive the no.
    if (!rn_plant("/etc/testdata/rn/f", omega, sizeof(omega) - 1))
        RN_FAIL("could not plant rn/f\n");
    if (kRootFilesystem->fops->rename("/etc/testdata/rn/f", "/etc/testdata/rn2",
                                      kRootFilesystem) == 0)
        RN_FAIL("rename replaced a DIRECTORY — the ruling forbids it\n");
    if (kRootFilesystem->dops->stat("/etc/testdata/rn/f", &de, kRootFilesystem) != 0)
        RN_FAIL("refused rename still consumed the source rn/f\n");
    if (kRootFilesystem->dops->stat("/etc/testdata/rn2", &de, kRootFilesystem) != 0
        || !(de.flags & OS64_DE_DIR))
        RN_FAIL("refused rename damaged the destination directory rn2\n");

    // 6. An open SOURCE is NOT a refusal — revised 2026-08-16 with the orphan
    //    slice. This step originally asserted the opposite, and it FAILED the
    //    first boot after the rule changed, which is exactly what a test that
    //    encodes a ruling is for. The rule it encodes now: an ext2 handle
    //    holds an INODE NUMBER, not a path, so a reader cannot tell that its
    //    file was renamed and has nothing to be protected from. The reader
    //    must keep reading, through the rename, without interruption.
    //    (Directories still refuse — a directory handle is mid-walk. The
    //    open-DESTINATION half of the ruling is test_ext2_orphan's job.)
    if (is_ext2) {
        vfs_file_t *held = NULL;
        if (kRootFilesystem->fops->open(&held, "/etc/testdata/rn/f", "r", kRootFilesystem) != 0)
            RN_FAIL("could not open rn/f to hold it\n");
        if (kRootFilesystem->fops->rename("/etc/testdata/rn/f",
                                          "/etc/testdata/rn/g", kRootFilesystem) != 0) {
            kRootFilesystem->fops->close(held);
            RN_FAIL("refused to rename a file that was merely OPEN — a reader holds an inode, not a name\n");
        }
        // The holder reads on, oblivious, from the same inode under its new name.
        n = kRootFilesystem->fops->read(held, buf, sizeof(buf));
        kRootFilesystem->fops->close(held);
        if (n != (int)(sizeof(omega) - 1) || memcmp(buf, omega, sizeof(omega) - 1) != 0)
            RN_FAIL("the held handle lost its bytes across a rename of its own file (n=%d)\n", n);
        if (kRootFilesystem->dops->stat("/etc/testdata/rn/f", &de, kRootFilesystem) == 0)
            RN_FAIL("old name rn/f still resolves after renaming an open file\n");
        if (kRootFilesystem->dops->stat("/etc/testdata/rn/g", &de, kRootFilesystem) != 0)
            RN_FAIL("new name rn/g missing after renaming an open file\n");
        kRootFilesystem->fops->rm("/etc/testdata/rn/g", kRootFilesystem);

        // A DIRECTORY held open still refuses to move: its reader is walking
        // the very blocks whose context the move changes.
        vfs_directory_t *heldDir = NULL;
        if (kRootFilesystem->dops->open(&heldDir, "/etc/testdata/rn2", kRootFilesystem) == 0) {
            int rcDir = kRootFilesystem->fops->rename("/etc/testdata/rn2",
                                                      "/etc/testdata/rn2moved", kRootFilesystem);
            kRootFilesystem->dops->close(heldDir);
            if (rcDir == 0)
                RN_FAIL("moved a directory that another handle was reading\n");
            if (kRootFilesystem->dops->stat("/etc/testdata/rn2", &de, kRootFilesystem) != 0)
                RN_FAIL("busy-refused directory rename consumed the source anyway\n");
        }
    } else {
        kRootFilesystem->fops->rm("/etc/testdata/rn/f", kRootFilesystem);
    }

    // 7. Move a whole DIRECTORY, and prove its contents came with it. One
    //    read through the new path exercises the new parent's entry and the
    //    moved directory's rewritten ".." at the same time.
    if (kRootFilesystem->fops->rename("/etc/testdata/rn2", "/etc/testdata/rn/sub",
                                      kRootFilesystem) != 0)
        RN_FAIL("directory rename rn2 -> rn/sub failed\n");
    if (kRootFilesystem->dops->stat("/etc/testdata/rn/sub", &de, kRootFilesystem) != 0
        || !(de.flags & OS64_DE_DIR))
        RN_FAIL("moved directory is not a directory at its new path\n");
    n = rn_slurp("/etc/testdata/rn/sub/c", buf, sizeof(buf));
    if (n != (int)(sizeof(alpha) - 1) || memcmp(buf, alpha, sizeof(alpha) - 1) != 0)
        RN_FAIL("moved directory's contents unreachable at the new path (n=%d)\n", n);

    // 8. A directory may not move into its own descendant. Attempted on ext2
    //    only — on FAT this would be asking the lifeboat to corrupt itself to
    //    see whether it says no.
    if (is_ext2) {
        if (kRootFilesystem->fops->rename("/etc/testdata/rn", "/etc/testdata/rn/sub/loop",
                                          kRootFilesystem) == 0)
            RN_FAIL("renamed a directory into its own descendant — that's a detached loop\n");
        if (kRootFilesystem->dops->stat("/etc/testdata/rn/sub/c", &de, kRootFilesystem) != 0)
            RN_FAIL("the refused loop rename damaged the tree\n");
    }

    // Tidy the corner: put rn2 back where the next boot expects to find
    // nothing, and take the litter with us.
    kRootFilesystem->fops->rm("/etc/testdata/rn/sub/c", kRootFilesystem);
    kRootFilesystem->fops->rm("/etc/testdata/rn/sub", kRootFilesystem);
    kRootFilesystem->fops->rm("/etc/testdata/rn", kRootFilesystem);

    printd(DEBUG_TESTS, "\tPASS: test_vfs_rename (%s: plain, atomic replace, move, directory move, open-source survives, %u refusals)\n",
           is_ext2 ? "ext2" : "FAT", is_ext2 ? 4u : 2u);
    return true;
}
#undef RN_FAIL

// ── test_ext2_orphan ────────────────────────────────────────────────────────
// The orphan chain's proving ground (2026-08-16). This test exists because
// THE FEATURE IS INVISIBLE: unlinking a file somebody still has open looks,
// from every outside angle, exactly like unlinking a file nobody has open.
// The difference is entirely in whether the storage came back — so this
// MEASURES the free counters rather than trusting that a teardown ran.
//
// Three claims, in order of how much it would hurt to get wrong:
//
//   1. THE READER KEEPS READING. A handle held across the replacement still
//      returns the OLD bytes. That is the whole point: a running /bin/husk
//      must keep demand-paging its own image after os64get has put a new one
//      at that name.
//   2. NOTHING LEAKS. Free inodes and free blocks return to EXACTLY their
//      starting values once the handle closes. Not "roughly", not "did not
//      grow" — equal. A leak here is silent forever and compounds once per
//      upgrade, which is precisely what an in-memory orphan list would have
//      risked and why the list is on disk.
//   3. THE NEW NAME IS THE NEW FILE, the instant the rename returns, even
//      though the displaced storage lives on a while longer.
//
// The real power-loss case remains a hand-driven two-boot procedure
// (VERIFICATION.md), but the failure half of mount replay IS exercised here:
// this mount owns a private copy of its block-operation table, so the test can
// fail one chosen metadata write, restore the real callback immediately, and
// ask fops->mounted to perform the same replay a boot performs.

// ext2_orphan test's block-write fault seam. These globals belong exclusively
// to test_ext2_orphan and are live only around its synchronous close/replay
// calls; ext2's write_lock keeps another writer on this mount from entering
// while the chosen operation is in flight.
static size_t (*sOrphanRealWrite)(void *, uint64_t, const void *, uint64_t);
static uint32_t sOrphanWriteCount;
static uint32_t sOrphanFailWrite;

static size_t orphan_test_write(void *device, uint64_t sector,
                                const void *buffer, uint64_t sector_count)
{
    sOrphanWriteCount++;
    // The instrument for re-choreographing this test: the write numbers the
    // stages below fail and expect are a script of the release path's
    // command stream, and any change to that stream (the 2026-08-29 batch
    // rewrote it) is read off this line against the image's group layout.
    printd(DEBUG_TESTS, "\torphan seam: write %u = LBA %lu x%lu%s\n",
           sOrphanWriteCount, sector, sector_count,
           sOrphanWriteCount == sOrphanFailWrite ? " (FAILED by the seam)" : "");
    if (sOrphanWriteCount == sOrphanFailWrite)
        return 1;
    return sOrphanRealWrite(device, sector, buffer, sector_count);
}

static bool orphan_test_fail_write(vfs_filesystem_t *fs, uint32_t nth)
{
    if (fs == NULL || fs->bops == NULL || fs->bops->write == NULL ||
        sOrphanRealWrite != NULL || nth == 0)
        return false;
    sOrphanRealWrite = fs->bops->write;
    sOrphanWriteCount = 0;
    sOrphanFailWrite = nth;
    fs->bops->write = orphan_test_write;
    return true;
}

static uint32_t orphan_test_restore_write(vfs_filesystem_t *fs)
{
    uint32_t writes = sOrphanWriteCount;
    fs->bops->write = sOrphanRealWrite;
    sOrphanRealWrite = NULL;
    sOrphanWriteCount = 0;
    sOrphanFailWrite = 0;
    return writes;
}

#define OR_FAIL(...) do { \
        printf("FAIL ext2_orphan: " __VA_ARGS__); \
        printd(DEBUG_TESTS, "\tFAIL: test_ext2_orphan - " __VA_ARGS__); \
        return false; \
    } while (0)

static bool test_ext2_orphan(void)
{
    if (kRootFilesystem == NULL || kRootFilesystem->fops == NULL ||
        kRootFilesystem->fops->write == NULL || kRootFilesystem->fops->rename == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_ext2_orphan (root not writable)\n");
        return true;
    }
    // ext2 only: FAT has no inode to orphan and no open-inode count. The
    // op-table identity is the discriminator (same idiom as test_vfs_rename).
    if (kRootFilesystem->fops->rename != ext2_rw_fops.rename) {
        printd(DEBUG_TESTS, "\tSKIP: test_ext2_orphan (root is not ext2)\n");
        return true;
    }

    static const char oldBytes[] = "THE-OLD-BINARY";
    static const char newBytes[] = "THE-NEW-BINARY";
    char buf[64];
    os64_dirent_t de;

    static const char *dirs[] = { "/etc", "/etc/testdata", "/etc/testdata/orph" };
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        char pathbuf[40];
        sprintf(pathbuf, "%s", dirs[i]);
        kRootFilesystem->dops->mkdir(pathbuf, kRootFilesystem);
        if (kRootFilesystem->dops->stat(dirs[i], &de, kRootFilesystem) != 0
            || !(de.flags & OS64_DE_DIR))
            OR_FAIL("provision: %s is not a directory\n", dirs[i]);
    }
    kRootFilesystem->fops->rm("/etc/testdata/orph/victim", kRootFilesystem);
    kRootFilesystem->fops->rm("/etc/testdata/orph/replacement", kRootFilesystem);
    kRootFilesystem->fops->rm("/etc/testdata/orph/retry_victim", kRootFilesystem);
    kRootFilesystem->fops->rm("/etc/testdata/orph/retry_replacement", kRootFilesystem);

    // THE BASELINE, taken after that cleanup so a previous boot's leftovers
    // cannot masquerade as this run's leak.
    uint32_t inodes0 = ext2_free_inodes(kRootFilesystem);
    uint32_t blocks0 = ext2_free_blocks(kRootFilesystem);
    if (inodes0 == 0 || blocks0 == 0)
        OR_FAIL("free-space accessors returned 0 — wrong filesystem, or a broken accessor\n");

    // The victim: the file standing in for a running program's binary. Make
    // it thirteen blocks so its map crosses the twelve direct slots and the
    // orphan teardown MUST visit an indirect block. The first bytes remain
    // distinctive for the held-reader assertion below.
    vfs_file_t *f = NULL;
    if (kRootFilesystem->fops->open(&f, "/etc/testdata/orph/victim", "c", kRootFilesystem) != 0)
        OR_FAIL("could not create the victim\n");
    uint32_t orphan_data_blocks = 13;
    uint8_t *orphan_data = kmalloc((size_t)kRootFilesystem->blockSize);
    if (orphan_data == NULL) {
        kRootFilesystem->fops->close(f);
        OR_FAIL("could not allocate the indirect-block fixture\n");
    }
    memset(orphan_data, 0xA5, (size_t)kRootFilesystem->blockSize);
    memcpy(orphan_data, oldBytes, sizeof(oldBytes) - 1);
    for (uint32_t i = 0; i < orphan_data_blocks; i++) {
        int wrote = kRootFilesystem->fops->write(
            f, orphan_data, (size_t)kRootFilesystem->blockSize);
        if (wrote != kRootFilesystem->blockSize) {
            kRootFilesystem->fops->close(f);
            kfree(orphan_data);
            OR_FAIL("could not write indirect-block fixture block %u (wrote %d)\n", i, wrote);
        }
    }
    kfree(orphan_data);
    kRootFilesystem->fops->close(f);
    f = NULL;

    // Its replacement, staged under another name exactly the way os64get will.
    if (kRootFilesystem->fops->open(&f, "/etc/testdata/orph/replacement", "c", kRootFilesystem) != 0)
        OR_FAIL("could not create the replacement\n");
    kRootFilesystem->fops->write(f, newBytes, sizeof(newBytes) - 1);
    kRootFilesystem->fops->close(f);
    f = NULL;

    // NOW HOLD THE VICTIM OPEN — this handle is the running program.
    // Hold it through a PRIVATE copy of the mount. A last-close release that
    // stops half-done now demotes the mount it ran on (a bitmap bit already
    // clear means the allocator could hand that storage to a live file), and
    // ext2_close demotes whatever mount the HANDLE was opened on — so this
    // copy takes the hit and the rest of the suite keeps its writable root.
    // Same trick, same reason, as test_ext2_readonly_demotion.
    // DECLARE THE DRILL. Everything below deliberately drives a real
    // demotion path, and the alarms it raises are written for an operator
    // watching a real disk get taken away. This routes them to the log for
    // the duration; a REAL demotion still reaches the glass. The runner
    // clears the flag when this test returns, however it returns (vfs.h).
    kTestingExpectedNoise = true;
    vfs_filesystem_t reap_mount = *kRootFilesystem;
    vfs_file_operations_t reap_fops = *kRootFilesystem->fops;
    vfs_directory_operations_t reap_dops = *kRootFilesystem->dops;
    reap_mount.fops = &reap_fops;
    reap_mount.dops = &reap_dops;

    vfs_file_t *held = NULL;
    if (reap_mount.fops->open(&held, "/etc/testdata/orph/victim", "r", &reap_mount) != 0)
        OR_FAIL("could not open the victim to hold it\n");

    // The replacement lands on the victim's name while that handle is live.
    // Before 2026-08-16 this refused outright.
    if (kRootFilesystem->fops->rename("/etc/testdata/orph/replacement",
                                      "/etc/testdata/orph/victim", kRootFilesystem) != 0) {
        kRootFilesystem->fops->close(held);
        OR_FAIL("rename onto an OPEN destination was refused — the orphan path never ran\n");
    }

    // CLAIM 1: the holder still reads the OLD bytes. Its inode has no name
    // any more; it does not care, and must not.
    int n = kRootFilesystem->fops->read(held, buf, sizeof(buf));
    if (n < (int)(sizeof(oldBytes) - 1) || memcmp(buf, oldBytes, sizeof(oldBytes) - 1) != 0) {
        kRootFilesystem->fops->close(held);
        OR_FAIL("the held handle stopped reading its own bytes (n=%d) — the orphan died early\n", n);
    }

    // CLAIM 3: the NAME already resolves to the new file, and the staging
    // name is gone.
    n = rn_slurp("/etc/testdata/orph/victim", buf, sizeof(buf));
    if (n != (int)(sizeof(newBytes) - 1) || memcmp(buf, newBytes, sizeof(newBytes) - 1) != 0) {
        kRootFilesystem->fops->close(held);
        OR_FAIL("the victim's NAME does not hold the new bytes (n=%d)\n", n);
    }
    if (kRootFilesystem->dops->stat("/etc/testdata/orph/replacement", &de, kRootFilesystem) == 0) {
        kRootFilesystem->fops->close(held);
        OR_FAIL("the staged name survived the rename\n");
    }

    // The orphan's storage must still be OUT. Without this check, a rename
    // that simply freed the inode too early could pass Claim 1 on luck (the
    // blocks would not have been overwritten yet).
    if (ext2_free_inodes(kRootFilesystem) == inodes0 &&
        ext2_free_blocks(kRootFilesystem) == blocks0) {
        kRootFilesystem->fops->close(held);
        OR_FAIL("free space is already back at baseline while the handle is OPEN — nothing was orphaned\n");
    }

    // The program exits. The release runs through one batch: every data
    // block's bit — twelve direct and the indirect child — clears in the
    // batch's bitmap copy and lands in ONE bitmap write (write 1) when the
    // indirect leaf's subtree is published; fail the group-descriptor count
    // that follows at write 2. Write 3 persists the retry map. The next
    // replay must see thirteen already-clear bits and reconcile both count
    // ledgers without counting any of them twice.
    uint32_t orphan_inodes = ext2_free_inodes(kRootFilesystem);
    uint32_t orphan_blocks = ext2_free_blocks(kRootFilesystem);
    if (!orphan_test_fail_write(kRootFilesystem, 2)) {
        kRootFilesystem->fops->close(held);
        OR_FAIL("could not install the release-write fault seam\n");
    }
    kRootFilesystem->fops->close(held);
    held = NULL;
    uint32_t injected_writes = orphan_test_restore_write(kRootFilesystem);
    if (injected_writes != 3)
        OR_FAIL("failed release made %u metadata writes, expected 3 (bitmap, failed GDT, retry inode)\n",
                injected_writes);
    if (ext2_free_inodes(kRootFilesystem) != orphan_inodes ||
        ext2_free_blocks(kRootFilesystem) != orphan_blocks + orphan_data_blocks)
        OR_FAIL("failed last-close release did not leave exactly the %u data blocks bitmap-completed for replay (freed %u)\n",
                orphan_data_blocks, ext2_free_blocks(kRootFilesystem) - orphan_blocks);
    // THE AMBIGUOUS FREE, asserted: those blocks' bitmap bits are clear on
    // disk while the retry map still names them (the leaf's pointers were
    // never rewritten, the indirect root's own bit never cleared), so the
    // allocator must be stopped until replay — and only AFTER write 3 put
    // the map somewhere replay can find it. A generic "the free failed"
    // would have left this mount taking allocations that replay would later
    // hand back to the free pool.
    if (!reap_mount.read_only)
        OR_FAIL("half-completed last-close block release did not demote its mount\n");
    if (kRootFilesystem->read_only || kRootFilesystem->fops->write == NULL)
        OR_FAIL("last-close demotion escaped onto the real root mount\n");

    // Now exercise MOUNT REPLAY itself, including the indirect ordering.
    // Replay revisits every direct block and the indirect child and finds
    // each bit already clear, so the leaf's publication is a reconcile:
    // bitmap (1), GDT (2), superblock (3); the leaf's pointer clear is 4.
    // The indirect root's own release publishes at the batch's end, 5..7.
    // The zero map is write 8 and the inode bitmap is 9; fail its GDT count
    // at write 10. A clean retry must reconcile the already-free inode too.
    if (kRootFilesystem->fops->mounted == NULL)
        OR_FAIL("ext2 mount table has no replay callback\n");
    if (!orphan_test_fail_write(kRootFilesystem, 10))
        OR_FAIL("could not install the replay-write fault seam\n");
    // Another private mount, for the same reason and a sharper hazard: write 9
    // freed the inode BITMAP BIT, so that inode NUMBER is allocatable while the
    // orphan chain still names it for teardown. Hand it to a new file and the
    // next replay releases that file's storage. This replay must therefore end
    // with the mount demoted, not merely with an error returned.
    kTestingExpectedNoise = true;   // drill: this replay MUST end demoted
    vfs_filesystem_t replay_mount = *kRootFilesystem;
    vfs_file_operations_t replay_fops = *kRootFilesystem->fops;
    vfs_directory_operations_t replay_dops = *kRootFilesystem->dops;
    replay_mount.fops = &replay_fops;
    replay_mount.dops = &replay_dops;

    replay_mount.fops->mounted(&replay_mount);
    injected_writes = orphan_test_restore_write(kRootFilesystem);
    if (injected_writes != 10)
        OR_FAIL("failed indirect replay made %u metadata writes, expected 10 with orphan still linked\n",
                injected_writes);
    if (!replay_mount.read_only)
        OR_FAIL("half-completed orphan inode free did not demote its mount\n");
    if (kRootFilesystem->read_only || kRootFilesystem->fops->write == NULL)
        OR_FAIL("replay demotion escaped onto the real root mount\n");
    if (ext2_free_inodes(kRootFilesystem) != orphan_inodes + 1)
        OR_FAIL("failed mount replay did not leave exactly one bitmap-completed inode for retry\n");
    if (ext2_free_blocks(kRootFilesystem) != orphan_blocks + orphan_data_blocks + 1)
        OR_FAIL("failed indirect replay freed %u blocks, expected %u data blocks plus indirect root\n",
                ext2_free_blocks(kRootFilesystem) - orphan_blocks, orphan_data_blocks);

    // A clean retry is the next boot in miniature. It must find the STILL-
    // LINKED orphan, idempotently finish its inode release, and only then
    // remove the durable chain record.
    kRootFilesystem->fops->mounted(kRootFilesystem);
    if (ext2_free_inodes(kRootFilesystem) != orphan_inodes + 1 ||
        ext2_free_blocks(kRootFilesystem) != orphan_blocks + orphan_data_blocks + 1)
        OR_FAIL("clean mount replay did not finish the retained orphan\n");

    // CLAIM 2: exactly what was taken, given back. Drop the surviving name
    // too, so the net against baseline must be zero on both counters.
    kRootFilesystem->fops->rm("/etc/testdata/orph/victim", kRootFilesystem);
    uint32_t inodes1 = ext2_free_inodes(kRootFilesystem);
    uint32_t blocks1 = ext2_free_blocks(kRootFilesystem);
    if (inodes1 != inodes0)
        OR_FAIL("INODE LEAK: %u free before, %u after (%d lost)\n",
                inodes0, inodes1, (int)inodes0 - (int)inodes1);
    if (blocks1 != blocks0)
        OR_FAIL("BLOCK LEAK: %u free before, %u after (%d lost)\n",
                blocks0, blocks1, (int)blocks0 - (int)blocks1);

    // Closed-destination rename failure: the rename's own writes are 1..7;
    // the release then publishes all thirteen data bits — twelve direct and
    // the indirect child — as bitmap 8, GDT 9, superblock 10, and write 11
    // is the parent-pointer clear. Fail that clear after its children are
    // already free. The retry inode + orphan-head writes MUST be allowed to
    // land as writes 12 and 13 before the mount publishes read-only state.
    // (The seam prints its stream under DEBUG_TESTS; re-read it there when
    // the release path's command stream changes shape.)
    if (kRootFilesystem->fops->open(&f, "/etc/testdata/orph/retry_victim", "c",
                                    kRootFilesystem) != 0)
        OR_FAIL("could not create the closed retry victim\n");
    orphan_data = kmalloc((size_t)kRootFilesystem->blockSize);
    if (orphan_data == NULL) {
        kRootFilesystem->fops->close(f);
        OR_FAIL("could not allocate the closed retry fixture\n");
    }
    memset(orphan_data, 0x5A, (size_t)kRootFilesystem->blockSize);
    for (uint32_t i = 0; i < orphan_data_blocks; i++) {
        int wrote = kRootFilesystem->fops->write(
            f, orphan_data, (size_t)kRootFilesystem->blockSize);
        if (wrote != kRootFilesystem->blockSize) {
            kRootFilesystem->fops->close(f);
            kfree(orphan_data);
            OR_FAIL("could not write closed retry fixture block %u (wrote %d)\n", i, wrote);
        }
    }
    kfree(orphan_data);
    kRootFilesystem->fops->close(f);
    f = NULL;

    if (kRootFilesystem->fops->open(&f, "/etc/testdata/orph/retry_replacement", "c",
                                    kRootFilesystem) != 0)
        OR_FAIL("could not create the closed retry replacement\n");
    kRootFilesystem->fops->write(f, newBytes, sizeof(newBytes) - 1);
    kRootFilesystem->fops->close(f);
    f = NULL;

    kTestingExpectedNoise = true;   // drill: the shadow mount takes the hit
    vfs_filesystem_t shadow = *kRootFilesystem;
    vfs_file_operations_t shadow_fops = *kRootFilesystem->fops;
    vfs_directory_operations_t shadow_dops = *kRootFilesystem->dops;
    shadow.fops = &shadow_fops;
    shadow.dops = &shadow_dops;

    if (!orphan_test_fail_write(kRootFilesystem, 11))
        OR_FAIL("could not install the closed-rename release fault seam\n");
    int rename_rc = shadow.fops->rename("/etc/testdata/orph/retry_replacement",
                                        "/etc/testdata/orph/retry_victim", &shadow);
    injected_writes = orphan_test_restore_write(kRootFilesystem);
    if (rename_rc != 0)
        OR_FAIL("closed replacement rename failed before reaching recoverable teardown\n");
    if (injected_writes != 13)
        OR_FAIL("failed closed rename made %u metadata writes, expected 13 (failed parent clear, retry inode, orphan head)\n",
                injected_writes);
    if (!shadow.read_only)
        OR_FAIL("closed rename parent-clear failure did not demote its mount\n");
    if (kRootFilesystem->read_only || kRootFilesystem->fops->write == NULL)
        OR_FAIL("private fault-test demotion escaped onto the real root mount\n");

    // The real mount stands in for the reboot: it shares the durable orphan
    // chain but was not demoted by the private fault fixture.
    kRootFilesystem->fops->mounted(kRootFilesystem);
    kRootFilesystem->fops->rm("/etc/testdata/orph/retry_victim", kRootFilesystem);
    if (ext2_free_inodes(kRootFilesystem) != inodes0 ||
        ext2_free_blocks(kRootFilesystem) != blocks0)
        OR_FAIL("closed-rename retry replay did not return all storage to baseline\n");

    printd(DEBUG_TESTS, "\tPASS: test_ext2_orphan (indirect ordering + idempotent counts + half-completed block/inode frees demote + retry-map-before-demotion; %u inodes / %u blocks returned exactly)\n",
           inodes0, blocks0);
    return true;
}
#undef OR_FAIL

// A runtime demotion must be stronger than clearing the obvious operation
// slots: ext2_open_rw remains installed so reads can still be opened, and a
// callback or open handle may have retained a pre-demotion function pointer.
// Exercise a PRIVATE mount copy so the real test boot keeps its writable root.
static bool test_ext2_readonly_demotion(void)
{
    if (kRootFilesystem == NULL || kRootFilesystem->fops == NULL ||
        kRootFilesystem->fops->rename != ext2_rw_fops.rename) {
        printd(DEBUG_TESTS, "\tSKIP: test_ext2_readonly_demotion (root is not writable ext2)\n");
        return true;
    }

    static const char guard_path[] = "/etc/testdata/ro_guard";
    static const char created_path[] = "/etc/testdata/ro_created";
    static const char renamed_path[] = "/etc/testdata/ro_renamed";
    char mkdir_path[] = "/etc/testdata/ro_dir";
    static const char guard_bytes[] = "READ-ONLY-GUARD";
    os64_dirent_t de;
    bool ok = true;

#define ROD_FAIL(...) do { \
        printf("FAIL ext2_readonly_demotion: " __VA_ARGS__); \
        printd(DEBUG_TESTS, "\tFAIL: test_ext2_readonly_demotion - " __VA_ARGS__); \
        ok = false; \
    } while (0)

    kRootFilesystem->fops->rm(guard_path, kRootFilesystem);
    kRootFilesystem->fops->rm(created_path, kRootFilesystem);
    kRootFilesystem->fops->rm(renamed_path, kRootFilesystem);
    kRootFilesystem->fops->rm(mkdir_path, kRootFilesystem);

    vfs_file_t *file = NULL;
    if (kRootFilesystem->fops->open(&file, guard_path, "c", kRootFilesystem) != 0 ||
        kRootFilesystem->fops->write(file, guard_bytes, sizeof(guard_bytes) - 1) !=
            (int)(sizeof(guard_bytes) - 1)) {
        if (file != NULL)
            kRootFilesystem->fops->close(file);
        ROD_FAIL("could not provision guard file\n");
        return false;
    }
    kRootFilesystem->fops->close(file);

    kTestingExpectedNoise = true;   // drill: the shadow mount takes the hit
    vfs_filesystem_t shadow = *kRootFilesystem;
    vfs_file_operations_t shadow_fops = *kRootFilesystem->fops;
    vfs_directory_operations_t shadow_dops = *kRootFilesystem->dops;
    shadow.fops = &shadow_fops;
    shadow.dops = &shadow_dops;

    int (*saved_open)(vfs_file_t **, const char *, const char *, vfs_filesystem_t *) = shadow_fops.open;
    int (*saved_write)(vfs_file_t *, const void *, size_t) = shadow_fops.write;
    int (*saved_rm)(const char *, vfs_filesystem_t *) = shadow_fops.rm;
    int (*saved_rename)(const char *, const char *, vfs_filesystem_t *) = shadow_fops.rename;
    int (*saved_mkdir)(char *, vfs_filesystem_t *) = shadow_dops.mkdir;

    vfs_demote_mount_readonly(&shadow);
    if (!shadow.read_only || shadow.fops->write != NULL ||
        shadow.fops->sync != NULL || shadow.fops->flush != NULL ||
        shadow.fops->rm != NULL || shadow.fops->rename != NULL ||
        shadow.dops->mkdir != NULL)
        ROD_FAIL("demotion left a direct mutating operation slot installed\n");

    // Read-only open remains useful after demotion.
    file = NULL;
    if (shadow.fops->open == NULL ||
        shadow.fops->open(&file, guard_path, "r", &shadow) != 0) {
        ROD_FAIL("demotion blocked an ordinary read open\n");
    } else {
        if (saved_write(file, "X", 1) >= 0)
            ROD_FAIL("retained write callback wrote through a demoted mount\n");
        shadow.fops->close(file);
    }

    const char modes[] = { 'w', 'c', 'a' };
    for (unsigned i = 0; i < sizeof(modes); i++) {
        char mode[2] = { modes[i], '\0' };
        file = NULL;
        if (saved_open(&file, guard_path, mode, &shadow) == 0) {
            ROD_FAIL("retained open callback accepted mode '%c' after demotion\n", modes[i]);
            shadow.fops->close(file);
        }
    }
    file = NULL;
    if (saved_open(&file, created_path, "c", &shadow) == 0) {
        ROD_FAIL("retained open callback created a file after demotion\n");
        shadow.fops->close(file);
    }
    if (saved_mkdir(mkdir_path, &shadow) == 0)
        ROD_FAIL("retained mkdir callback mutated a demoted mount\n");
    if (saved_rename(guard_path, renamed_path, &shadow) == 0)
        ROD_FAIL("retained rename callback mutated a demoted mount\n");
    if (saved_rm(guard_path, &shadow) == 0)
        ROD_FAIL("retained rm callback mutated a demoted mount\n");

    char contents[32];
    int n = rn_slurp(guard_path, contents, sizeof(contents));
    if (n != (int)(sizeof(guard_bytes) - 1) ||
        memcmp(contents, guard_bytes, sizeof(guard_bytes) - 1) != 0)
        ROD_FAIL("guard file changed despite read-only demotion (n=%d)\n", n);
    if (kRootFilesystem->dops->stat(created_path, &de, kRootFilesystem) == 0 ||
        kRootFilesystem->dops->stat(renamed_path, &de, kRootFilesystem) == 0 ||
        kRootFilesystem->dops->stat(mkdir_path, &de, kRootFilesystem) == 0)
        ROD_FAIL("a refused create, rename, or mkdir left a namespace entry\n");

    kRootFilesystem->fops->rm(guard_path, kRootFilesystem);
    kRootFilesystem->fops->rm(created_path, kRootFilesystem);
    kRootFilesystem->fops->rm(renamed_path, kRootFilesystem);
    kRootFilesystem->fops->rm(mkdir_path, kRootFilesystem);

    if (ok)
        printd(DEBUG_TESTS, "\tPASS: test_ext2_readonly_demotion (read allowed; create/truncate/append/write/rm/rename/mkdir refused)\n");
    return ok;
#undef ROD_FAIL
}

// ── test_ext2_secondary_write ───────────────────────────────────────────────
// The ext2 WRITE driver's proving ground (2026-08-04 — the day os64 wrote
// its first ext2 byte). Runs against the WRITABLE SECONDARY ext2 mount
// (/ext2), which exists only on FAT-root boots — on ext2-root boots the
// ext2 partition IS the root (writable since the 2026-08-07 ratification;
// its write path is exercised by test_vfs_write_mkdir and the ext2_real
// self-provision phase) and there is no secondary to test, so this SKIPs
// honestly. The other judge is host-side: `make fsck-ext2` after this suite
// must stay green — e2fsck recomputes every structure this driver maintains.
//
// ES_FAIL reports on the glass AND the log (the 2026-08-08 silencer lesson)
// but does NOT return — unlike its MT_/E2_/VW_ cousins — because half these
// failure paths close an open handle before bailing; control flow stays
// with the caller.
#define ES_FAIL(...) do { \
        printf("FAIL ext2_secondary_write: " __VA_ARGS__); \
        printd(DEBUG_TESTS, "\tFAIL: ext2_secondary_write - " __VA_ARGS__); \
    } while (0)
static bool test_ext2_secondary_write(void)
{
    // Find the writable ext2 secondary in the mount table by its prefix
    // (the GPT partition name is "ext2", authored by the build).
    vfs_filesystem_t *fs = NULL;
    for (int i = 0; i < kMountCount; i++)
        if (strcmp(kMountTable[i].prefix, "/ext2") == 0)
        {
            fs = kMountTable[i].fs;
            break;
        }
    if (fs == NULL || fs->fops == NULL || fs->fops->write == NULL)
    {
        printd(DEBUG_TESTS, "\tSKIP: test_ext2_secondary_write (no writable /ext2 mount on this boot)\n");
        return true;
    }

    static const char msg1[] = "Hello from the write era";        // 24 bytes
    static const char msg2[] = "appended";                        // 8 bytes
    static const char msg3[] = "truncated and rewritten";         // 23 bytes
    char buf[64];
    vfs_file_t *f = NULL;

    // 1. Create, write, close, reopen, read back byte-exact.
    if (fs->fops->open(&f, "/__wtest.txt", "c", fs) != 0) {
        ES_FAIL("create /__wtest.txt failed\n");
        return false;
    }
    if (fs->fops->write(f, msg1, sizeof(msg1) - 1) != (int)(sizeof(msg1) - 1)) {
        ES_FAIL("write to fresh file failed\n");
        fs->fops->close(f);
        return false;
    }
    fs->fops->close(f);
    f = NULL;
    if (fs->fops->open(&f, "/__wtest.txt", "r", fs) != 0) {
        ES_FAIL("reopen after create failed\n");
        return false;
    }
    int n = fs->fops->read(f, buf, sizeof(buf));
    fs->fops->close(f);
    f = NULL;
    if (n != (int)(sizeof(msg1) - 1) || memcmp(buf, msg1, sizeof(msg1) - 1) != 0) {
        ES_FAIL("read-back mismatch (n=%d)\n", n);
        return false;
    }

    // 2. Append; verify the concatenation and the stat'd size.
    if (fs->fops->open(&f, "/__wtest.txt", "a", fs) != 0) {
        ES_FAIL("append open failed\n");
        return false;
    }
    if (fs->fops->write(f, msg2, sizeof(msg2) - 1) != (int)(sizeof(msg2) - 1)) {
        ES_FAIL("append write failed\n");
        fs->fops->close(f);
        return false;
    }
    fs->fops->close(f);
    f = NULL;
    os64_dirent_t de;
    if (fs->dops->stat("/__wtest.txt", &de, fs) != 0 ||
        de.size != (sizeof(msg1) - 1) + (sizeof(msg2) - 1)) {
        ES_FAIL("post-append size wrong (%lu)\n", de.size);
        return false;
    }

    // 3. Truncate via "w": size drops to 0 territory, then rewrite.
    if (fs->fops->open(&f, "/__wtest.txt", "w", fs) != 0) {
        ES_FAIL("truncating open failed\n");
        return false;
    }
    if (fs->dops->stat("/__wtest.txt", &de, fs) != 0 || de.size != 0) {
        ES_FAIL("size not 0 after truncate (%lu)\n", de.size);
        fs->fops->close(f);
        return false;
    }
    fs->fops->write(f, msg3, sizeof(msg3) - 1);
    fs->fops->close(f);
    f = NULL;
    if (fs->fops->open(&f, "/__wtest.txt", "r", fs) != 0) {
        ES_FAIL("reopen after truncate failed\n");
        return false;
    }
    n = fs->fops->read(f, buf, sizeof(buf));
    fs->fops->close(f);
    f = NULL;
    if (n != (int)(sizeof(msg3) - 1) || memcmp(buf, msg3, sizeof(msg3) - 1) != 0) {
        ES_FAIL("post-truncate read-back mismatch (n=%d)\n", n);
        return false;
    }

    // 4. Growth across the single-indirect boundary. At 1KB blocks the
    //    direct blocks end at byte 12,287; a 16KB file forces the indirect
    //    chain into existence. Self-describing 16-byte records, same idea
    //    as pattern.bin.
    if (fs->fops->open(&f, "/__wbig.bin", "c", fs) != 0) {
        ES_FAIL("create /__wbig.bin failed\n");
        return false;
    }
    char rec[17];
    for (uint32_t i = 0; i < 1024; i++) {
        sprintf(rec, "%08u:os64wr\n", i);     // exactly 16 bytes
        if (fs->fops->write(f, rec, 16) != 16) {
            ES_FAIL("pattern write failed at record %u\n", i);
            fs->fops->close(f);
            return false;
        }
    }
    fs->fops->close(f);
    f = NULL;
    if (fs->dops->stat("/__wbig.bin", &de, fs) != 0 || de.size != 16384) {
        ES_FAIL("/__wbig.bin size %lu != 16384\n", de.size);
        return false;
    }
    if (fs->fops->open(&f, "/__wbig.bin", "r", fs) != 0) {
        ES_FAIL("reopen /__wbig.bin failed\n");
        return false;
    }
    // The record straddling the direct/indirect boundary (bytes 12272-12303
    // cover 12287|12288) and the final record.
    fs->fops->seek(f, 12272, SEEK_SET);
    n = fs->fops->read(f, buf, 32);
    sprintf(rec, "%08u:os64wr\n", 767u);      // record 767 = bytes 12272..12287
    if (n != 32 || memcmp(buf, rec, 16) != 0) {
        ES_FAIL("boundary record 767 mismatch\n");
        fs->fops->close(f);
        return false;
    }
    sprintf(rec, "%08u:os64wr\n", 768u);      // record 768 = first indirect bytes
    if (memcmp(buf + 16, rec, 16) != 0) {
        ES_FAIL("boundary record 768 mismatch\n");
        fs->fops->close(f);
        return false;
    }
    fs->fops->seek(f, -16, SEEK_END);
    n = fs->fops->read(f, buf, 16);
    sprintf(rec, "%08u:os64wr\n", 1023u);
    if (n != 16 || memcmp(buf, rec, 16) != 0) {
        ES_FAIL("final record mismatch\n");
        fs->fops->close(f);
        return false;
    }
    fs->fops->close(f);
    f = NULL;

    // 5. A hole: seek far past end, write one record; the gap reads zeros.
    if (fs->fops->open(&f, "/__whole.bin", "c", fs) != 0) {
        ES_FAIL("create /__whole.bin failed\n");
        return false;
    }
    fs->fops->seek(f, 20480, SEEK_SET);
    sprintf(rec, "%08u:os64wr\n", 9999u);
    fs->fops->write(f, rec, 16);
    fs->fops->close(f);
    f = NULL;
    if (fs->dops->stat("/__whole.bin", &de, fs) != 0 || de.size != 20496) {
        ES_FAIL("hole-file size %lu != 20496\n", de.size);
        return false;
    }
    if (fs->fops->open(&f, "/__whole.bin", "r", fs) != 0) {
        ES_FAIL("reopen /__whole.bin failed\n");
        return false;
    }
    fs->fops->seek(f, 5000, SEEK_SET);
    n = fs->fops->read(f, buf, 16);
    bool zeros = (n == 16);
    for (int i = 0; zeros && i < 16; i++)
        if (buf[i] != 0)
            zeros = false;
    if (!zeros) {
        ES_FAIL("hole did not read as zeros\n");
        fs->fops->close(f);
        return false;
    }
    fs->fops->seek(f, 20480, SEEK_SET);
    n = fs->fops->read(f, buf, 16);
    fs->fops->close(f);
    f = NULL;
    if (n != 16 || memcmp(buf, rec, 16) != 0) {
        ES_FAIL("post-hole record mismatch\n");
        return false;
    }

    // 6. mkdir; a file inside proves it's a real directory; readdir sees the
    //    file and (Plan 9 doctrine) no dot entries.
    if (fs->dops->mkdir("/__wdir", fs) != 0) {
        ES_FAIL("mkdir /__wdir failed\n");
        return false;
    }
    if (fs->fops->open(&f, "/__wdir/inner.txt", "c", fs) != 0) {
        ES_FAIL("create in new dir failed\n");
        return false;
    }
    fs->fops->write(f, msg1, sizeof(msg1) - 1);
    fs->fops->close(f);
    f = NULL;
    vfs_directory_t *d = NULL;
    if (fs->dops->open(&d, "/__wdir", fs) != 0) {
        ES_FAIL("opendir /__wdir failed\n");
        return false;
    }
    bool saw_inner = false, saw_dots = false;
    while (fs->dops->read(d, &de) == 1) {
        if (strcmp(de.name, "inner.txt") == 0)
            saw_inner = true;
        if (de.name[0] == '.' )
            saw_dots = true;
    }
    fs->dops->close(d);
    if (!saw_inner || saw_dots) {
        ES_FAIL("dir listing wrong (inner=%d dots=%d)\n",
               saw_inner, saw_dots);
        return false;
    }

    // 7. The one removal verb, all four verdicts: non-empty dir refused,
    //    nonexistent refused, file removed, then-empty dir removed.
    if (fs->fops->rm("/__wdir", fs) == 0) {
        ES_FAIL("rm of NON-empty dir succeeded (must refuse)\n");
        return false;
    }
    if (fs->fops->rm("/__no_such_thing", fs) == 0) {
        ES_FAIL("rm of nonexistent path succeeded\n");
        return false;
    }
    if (fs->fops->rm("/__wdir/inner.txt", fs) != 0 ||
        fs->dops->stat("/__wdir/inner.txt", &de, fs) == 0) {
        ES_FAIL("rm file failed (or stat still sees it)\n");
        return false;
    }
    if (fs->fops->rm("/__wdir", fs) != 0 ||
        fs->dops->stat("/__wdir", &de, fs) == 0) {
        ES_FAIL("rm empty dir failed (or stat still sees it)\n");
        return false;
    }

    // 8. THE ORPHAN CONTRACT — ruling 5 (2026-08-04) as REVISED 2026-08-16.
    //
    //    Ruling 5 made rm refuse ANY open file, and this step asserted
    //    exactly that. The orphan slice superseded it: an open REGULAR file
    //    is unlinked NOW and its storage released at LAST CLOSE, through
    //    ext2's on-disk orphan chain. That revision is the entire reason
    //    `os64 refresh` can replace /bin/husk while husk is running.
    //
    //    What SURVIVES of ruling 5, both asserted below: a truncating "w"
    //    open of an open file still refuses (the reader would watch its
    //    blocks recycle underneath it), and an open DIRECTORY still refuses
    //    to be removed — nobody has asked, and a handle mid-walk through a
    //    directory's blocks is a harder promise to keep.
    //
    //    WHY THIS WAS WRONG FOR FOUR DAYS (fixed 2026-08-20): the test only
    //    runs where /ext2 is a writable SECONDARY mount — i.e. a FAT-root
    //    boot — and the arc that changed the rule was verified on ext2 root,
    //    where this whole test skips. Booting "/QEMU Boot (FAT root + full
    //    test suite)" before blessing a kernel change is not ceremony; this
    //    is the failure it exists to catch.
    if (fs->fops->open(&f, "/__wtest.txt", "r", fs) != 0) {
        ES_FAIL("hold-open failed\n");
        return false;
    }

    // Ruling 5's surviving half, checked BEFORE the rm below — afterwards
    // there is no name left to open.
    vfs_file_t *f2 = NULL;
    if (fs->fops->open(&f2, "/__wtest.txt", "w", fs) == 0) {
        ES_FAIL("truncating open of an OPEN file succeeded\n");
        fs->fops->close(f2);
        fs->fops->close(f);
        return false;
    }

    // The NAME goes now...
    if (fs->fops->rm("/__wtest.txt", fs) != 0) {
        ES_FAIL("rm of an OPEN regular file refused (must orphan it)\n");
        fs->fops->close(f);
        return false;
    }
    if (fs->dops->stat("/__wtest.txt", &de, fs) == 0) {
        ES_FAIL("orphaned file still answers to its name\n");
        fs->fops->close(f);
        return false;
    }

    // ...and the STORAGE stays until the last holder lets go. This read is
    // the whole point of the orphan chain: a program whose binary was just
    // replaced keeps executing the bytes it already opened.
    n = fs->fops->read(f, buf, sizeof(buf));
    if (n != (int)(sizeof(msg3) - 1) || memcmp(buf, msg3, sizeof(msg3) - 1) != 0) {
        ES_FAIL("orphan unreadable through its open handle (n=%d)\n", n);
        fs->fops->close(f);
        return false;
    }
    fs->fops->close(f);          // last close — the reap happens HERE
    f = NULL;

    // And the name is gone for good: nothing left for a second rm to find.
    if (fs->fops->rm("/__wtest.txt", fs) == 0) {
        ES_FAIL("rm of an already-orphaned name succeeded\n");
        return false;
    }

    // Ruling 5's other survivor, which nothing covered before: the
    // assertion this replaces was busy testing a rule that no longer exists.
    if (fs->dops->mkdir("/__wdir2", fs) != 0) {
        ES_FAIL("mkdir /__wdir2 failed\n");
        return false;
    }
    if (fs->dops->open(&d, "/__wdir2", fs) != 0) {
        ES_FAIL("opendir /__wdir2 failed\n");
        return false;
    }
    if (fs->fops->rm("/__wdir2", fs) == 0) {
        ES_FAIL("rm of an OPEN directory succeeded (must refuse)\n");
        fs->dops->close(d);
        return false;
    }
    fs->dops->close(d);
    if (fs->fops->rm("/__wdir2", fs) != 0) {
        ES_FAIL("rm of /__wdir2 after close failed\n");
        return false;
    }

    // 9. Cleanup doubles as coverage: freeing /__wbig.bin tears down a real
    //    indirect chain, /__whole.bin a sparse map. e2fsck audits the wake.
    if (fs->fops->rm("/__wbig.bin", fs) != 0 || fs->fops->rm("/__whole.bin", fs) != 0) {
        ES_FAIL("cleanup rm failed\n");
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_ext2_secondary_write (create/append/truncate/indirect/hole/mkdir/rm/orphan/busy-refusal)\n");
    return true;
}
#undef ES_FAIL

// ── test_console_read_deadline ──────────────────────────────────────────────
// The read-patience contract (ruled 2026-08-05), proved at the console layer:
// a poll never parks, a timed read gives up on schedule. Deliberately
// TOLERANT of a human typing during boot — a physical keyboard's queue can't
// be asserted empty, so "a byte arrived" is always an acceptable outcome;
// what the test refuses to accept is a poll that BLOCKS or a deadline that
// doesn't expire.
//
// EVERY BYTE THIS PROBE CATCHES GOES BACK (console_unread). The first
// version shrugged — "any byte this test eats was typed before husk existed
// to want it" — and that shrug stole the first few characters of Chris's
// type-ahead on every boot for weeks; the rc feature took the blame because
// they shipped the same day. A probe that must consume to observe
// un-consumes on the way out; type-ahead reaches the prompt intact.
static bool test_console_read_deadline(void)
{
    char c;

    // 1. The poll gait: deadline already now. Must return, byte or verdict,
    //    without parking — bounded by one tick of scheduler jitter, not by
    //    the console's one-second backstop nap.
    uint64_t t0 = kTicksSinceStart;
    long r = console_read_deadline(&c, 1, kTicksSinceStart);
    uint64_t elapsed = kTicksSinceStart - t0;
    if (r == 1)
        console_unread(c);
    if (r != CONSOLE_READ_TIMEOUT && r != 1) {
        printd(DEBUG_TESTS, "\tFAIL: console_read_deadline - poll returned %ld (want byte or timeout)\n", r);
        return false;
    }
    if (elapsed > 2) {
        printd(DEBUG_TESTS, "\tFAIL: console_read_deadline - poll took %lu ticks (a poll must not wait)\n", elapsed);
        return false;
    }

    // 2. The timed gait: 3 ticks of patience. On the quiet path the verdict
    //    must land at the deadline — not early (patience is a promise) and
    //    not a backstop-second late (the shortened nap must hold).
    //    (If gait 1 put a byte back, this read returns it instantly — which
    //    is the pushback contract working, and an acceptable outcome here.)
    t0 = kTicksSinceStart;
    r = console_read_deadline(&c, 1, kTicksSinceStart + 3);
    elapsed = kTicksSinceStart - t0;
    if (r == 1)
        console_unread(c);
    if (r == CONSOLE_READ_TIMEOUT) {
        if (elapsed < 3 || elapsed > 20) {
            printd(DEBUG_TESTS, "\tFAIL: console_read_deadline - 3-tick patience expired after %lu ticks\n", elapsed);
            return false;
        }
    } else if (r != 1) {
        printd(DEBUG_TESTS, "\tFAIL: console_read_deadline - timed read returned %ld\n", r);
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_console_read_deadline (poll + 3-tick patience; anything caught was put back)\n");
    return true;
}


// (A dedicated /bin/hello test lived here briefly during userland bring-up;
// removed as redundant — ring3_syscall_smoke and ring3_exit_by_return already
// cover load-run-exit at CPL 3, and the HELLO boot-flow launch exercises the
// real app path. /bin/hello stays on the image for that launch.)

// ── net tests ────────────────────────────────────────────────────────────────

typedef struct arp_pending_order_state
{
    uint32_t next_hop;
    uint8_t peer_mac[NET_MAC_LEN];
    uint32_t arp_frames;
    uint32_t ipv4_frames;
    bool reply_injected;
} arp_pending_order_state_t;

// Model the shortest possible ARP round trip: the reply arrives from inside
// the fake NIC's transmit callback, before arp_send_request can return. This
// deterministically exercises the SMP window where another core may process a
// reply immediately after the request reaches the wire.
static int32_t arp_pending_order_transmit(net_device_t *dev, const void *frame,
                                          uint16_t length)
{
    arp_pending_order_state_t *state = (arp_pending_order_state_t*)dev->driver_data;
    const uint8_t *bytes = (const uint8_t*)frame;

    if (length < ETH_HDR_LEN)
        return -1;

    uint16_t ethertype = net_read16(bytes + 12);
    if (ethertype == ETH_TYPE_IPV4) {
        state->ipv4_frames++;
        return 0;
    }
    if (ethertype != ETH_TYPE_ARP || state->reply_injected)
        return 0;

    state->arp_frames++;
    state->reply_injected = true;

    uint8_t reply[ARP_PKT_LEN];
    net_write16(reply + 0, 1);
    net_write16(reply + 2, ETH_TYPE_IPV4);
    reply[4] = NET_MAC_LEN;
    reply[5] = 4;
    net_write16(reply + 6, ARP_OPER_REPLY);
    memcpy(reply + 8, state->peer_mac, NET_MAC_LEN);
    net_write32(reply + 14, state->next_hop);
    memcpy(reply + 18, dev->mac, NET_MAC_LEN);
    net_write32(reply + 24, kNetIPv4Address);
    arp_input(dev, reply, sizeof(reply));
    return 0;
}

static bool test_net_arp_pending_order(void)
{
    arp_pending_order_state_t state = {
        .next_hop = kNetIPv4Gateway,
        .peer_mac = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02},
    };
    net_operations_t ops = { .transmit = arp_pending_order_transmit };
    net_device_t dev = {
        .mac = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01},
        .mtu = 1500,
        .link_up = true,
        .ops = &ops,
        .driver_data = &state,
    };
    const uint8_t payload[] = {0x52, 0x41, 0x43, 0x45};

    // Force the miss that enters the waiting room. The fake reply teaches the
    // cache synchronously; flush afterward so this isolated test cannot make
    // the real-wire ARP tests pass on synthetic evidence.
    arp_cache_flush();
    uint64_t parked_before = kIPv4Stats.tx_parked_for_arp;
    uint64_t dropped_before = kIPv4Stats.tx_awaiting_arp;
    int32_t rc = ipv4_send_from(&dev, kNetIPv4Address, kNetIPv4Gateway,
                                IPV4_PROTO_UDP, payload, sizeof(payload));
    bool released_in_time = state.ipv4_frames == 1;
    bool accounting_ok = kIPv4Stats.tx_parked_for_arp == parked_before + 1 &&
                         kIPv4Stats.tx_awaiting_arp == dropped_before;

    // Under the buggy send-then-park ordering the reply has already gone by,
    // leaving a packet behind. Release it solely to keep a failing test from
    // contaminating later tests; the verdict above records whether it was late.
    if (!released_in_time)
        ipv4_arp_resolved(&dev, state.next_hop);
    arp_cache_flush();

    if (rc != -2 || state.arp_frames != 1 || !released_in_time || !accounting_ok) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_arp_pending_order - rc=%d arp=%u "
               "ipv4_before_cleanup=%u parked_delta=%lu dropped_delta=%lu\n",
               rc, state.arp_frames, released_in_time ? 1U : 0U,
               kIPv4Stats.tx_parked_for_arp - parked_before,
               kIPv4Stats.tx_awaiting_arp - dropped_before);
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_net_arp_pending_order (synchronous ARP reply released "
           "parked IPv4 packet; parked=1 dropped=0)\n");
    return true;
}

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
// ── When is a network test's world actually present? ────────────────────────
//
// The net tests were written against QEMU's slirp, which is a whole
// pretend internet in a process: it answers ARP, hands out DHCP leases,
// replies to pings, and hosts a gateway at 10.0.2.2. Every one of those is
// an ASSUMPTION, and on 2026-08-16 the P5 met a network where none of them
// hold — an isolated segment with one peer, no DHCP server, and no gateway
// at all. Four tests went red and stayed red on every boot.
//
// That is worse than it sounds. A suite with permanently-failing lines is a
// suite people stop reading, and this session proved twice over that the
// suite is what catches things (test_vfs_rename failed the instant a ruling
// changed, which is exactly what it was for). Red lines that mean "your
// network is different" drown the red lines that mean "you broke it".
//
// So these two predicates let a test say I CANNOT RUN HERE instead of I
// FAILED. The distinction is the whole point: a skip is honest, a failure
// is a claim about the code.
//
// A gateway is expected only when this boot actually has one: either the
// cmdline named it (GW=), or no static IP was given at all, in which case
// we are in the DHCP/NAT world where the convention default is real. A
// boot with IP= and no GW= — the P5's build segment — has a gateway
// address that is pure convention, and nothing lives there.
static bool net_test_has_gateway(void)
{
    return kNetGWString[0] != '\0' || kNetIPString[0] == '\0';
}

// DHCP is expected only when nobody configured the address by hand. IP= on
// the cmdline SUPPRESSES the DISCOVER outright (kernel.c), so a static boot
// failing a DHCP test is the test misreading a deliberate configuration as
// a malfunction.
static bool net_test_expects_dhcp(void)
{
    return kNetIPString[0] == '\0';
}

static bool test_net_wire(void)
{
    if (kNetDeviceCount == 0) {
        printd(DEBUG_TESTS, "\tSKIP: test_net_wire (no NIC — QEMU: -netdev user,id=n0 -device virtio-net-pci,netdev=n0)\n");
        return true;
    }

    if (!net_test_has_gateway()) {
        printd(DEBUG_TESTS, "\tSKIP: test_net_wire (no gateway on this segment — nothing would answer the ARP)\n");
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
    // OUR address and OUR gateway, not slirp's. These were hardcoded as
    // 10.0.2.15 and 10.0.2.2 until the P5 booted on a real segment and the
    // test spent every boot asking a machine that does not exist to
    // identify itself.
    net_write32(f + n, kNetIPv4Address);   n += 4;   // sender IP
    memset(f + n, 0x00, 6);                n += 6;   // target MAC: the question
    net_write32(f + n, kNetIPv4Gateway);   n += 4;   // target IP: the gateway

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
    if (!net_test_has_gateway()) {
        printd(DEBUG_TESTS, "\tSKIP: test_net_arp (no gateway on this segment to resolve)\n");
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
    if (!net_test_has_gateway()) {
        printd(DEBUG_TESTS, "\tSKIP: test_net_ping (no gateway on this segment to ping)\n");
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
    if (!net_test_expects_dhcp()) {
        printd(DEBUG_TESTS, "\tSKIP: test_net_dhcp (IP= was given, so the DISCOVER was never sent)\n");
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
    if (!net_test_has_gateway()) {
        printd(DEBUG_TESTS, "\tSKIP: test_net_icmp_conn (no gateway on this segment to echo — this one BLOCKS waiting for the reply)\n");
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
    if (!net_test_has_gateway()) {
        printd(DEBUG_TESTS, "\tSKIP: test_net_tcp_refused (no gateway on this segment to refuse the connection)\n");
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
    // Silence is the ROUTER's answer, not the stack's. A gateway that drops
    // SYNs to port 9 instead of resetting them (a firewall, or a busy home
    // router that rate-limits its own RSTs) times the dial out with nothing
    // to count — and a test that calls that a stack failure cries wolf
    // (the P5's gateway did it one boot in six). Refusal is asserted only
    // when there WAS a refusal; the timeout is reported and skipped.
    // THE VERDICT IS CONNECTION-LOCAL, and it had to become so when this test
    // moved to the LATE phase: `kTcpStats.connections_refused` is MACHINE-WIDE,
    // so any other program completing a refused dial inside our window moved
    // it. That turned a valid refusal into a failure (delta > 1) and a valid
    // timeout into a failure rather than a skip (delta != 0). `why` comes from
    // this connection's own `c->reset` and cannot be moved by anybody else, so
    // it answers the question the counter was only approximating. (Codex, PR
    // #42.) The counter still appears in the failure text below, where it is
    // context for a human rather than an assertion.
    if (why == OS64_NET_ERR_TIMEOUT) {
        printd(DEBUG_TESTS, "\tSKIP: test_net_tcp_refused (the gateway answered port 9 with silence, not RST — "
               "timed out after %lu ticks; nothing to assert about the stack)\n", elapsed);
        return true;
    }
    // The dial must not just fail — it must fail with the RIGHT STORY.
    // A ping author staring at a bare -1 is how this assertion got here.
    if (why != OS64_NET_ERR_REFUSED) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_tcp_refused - failed with why=%ld, "
               "want OS64_NET_ERR_REFUSED (%d)\n", why, OS64_NET_ERR_REFUSED);
        return false;
    }
    // A refusal must be an ANSWER (an RST), not the connect timeout — fast
    // failure is the observable difference, and the timeout is 10s. AT LEAST
    // ours, not EXACTLY ours: the counter is machine-wide, so demanding it
    // moved by exactly one made this test fail whenever another program's
    // dial was refused in the same window. `why` above already proved THIS
    // connection got an RST; what is left for the counter to prove is only
    // that the stack counted the thing it just did.
    if (kTcpStats.connections_refused < refused_before + 1) {
        printd(DEBUG_TESTS, "\tFAIL: test_net_tcp_refused - the dial reported REFUSED but "
               "no RST was counted (refused=%lu timeouts=%lu resets=%lu, took %lu ticks)\n",
               kTcpStats.connections_refused, kTcpStats.connect_timeouts,
               kTcpStats.resets_received, elapsed);
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_net_tcp_refused (closed port answered with RST in %lu ticks)\n",
           elapsed);
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


// The buffer cache, two claims tested (block_cache.h carries the design):
// (1) HIT RATE — reading the same file twice serves the second pass mostly
// from memory; (2) COHERENCE, the one that matters more — a write through
// the shim INVALIDATES cached lines, so a reader can never be handed
// yesterday's bytes. A cache that's fast and stale is a bug with good PR.
static bool test_block_cache(void)
{
    if (!block_cache_is_active()) {
        printd(DEBUG_TESTS, "\tSKIP: test_block_cache (cache disabled or no cacheable device)\n");
        return true;
    }
    if (kRootFilesystem == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_block_cache (no root filesystem)\n");
        return true;
    }

    // The cache attaches to REAL disks only (NVMe/SATA) — so on a
    // RAMDisk-root boot (the P5's self-contained ISO) it can be ACTIVE,
    // interposed on the machine's internal drive, while the root this test
    // reads from never touches it. Each claim therefore runs only when the
    // cache covers the specific device it exercises; "active" alone asked
    // the wrong question (the P5 found this, 2026-08-07: honest zero hits
    // reported as a failure).
    bool rootCovered = block_cache_covers(kRootFilesystem->block_device_info);

    char *chunk = kmalloc(65536);
    if (chunk == NULL)
        TEST_FAIL("kmalloc for read chunk failed");

    // ── Claim 1: second read of a binary is served warm ─────────────────────
    uint64_t missesCold = 0, missesWarm = 0, hitsWarm = 0;
    if (rootCovered)
    {
        block_cache_stats_t s0, s1, s2;
        for (int pass = 0; pass < 2; pass++)
        {
            block_cache_get_stats(pass == 0 ? &s0 : &s1);
            vfs_file_t *f = NULL;
            if (kRootFilesystem->fops->open(&f, "/bin/top", "r", kRootFilesystem) != 0 || f == NULL)
            {
                kfree(chunk);
                TEST_FAIL("open /bin/top failed");
            }
            int n;
            while ((n = f->fops->read(f, chunk, 65536)) > 0)
                ;
            f->fops->close(f);
        }
        block_cache_get_stats(&s2);

        missesCold = s1.misses - s0.misses;
        missesWarm = s2.misses - s1.misses;
        hitsWarm   = s2.hits - s1.hits;
        if (hitsWarm == 0)
        {
            kfree(chunk);
            TEST_FAIL("warm pass produced zero cache hits");
        }
        if (missesWarm > missesCold / 2)
        {
            // The warm pass should be nearly all hits; logd traffic on the data
            // disk can add a stray miss or two, hence /2 rather than zero.
            kfree(chunk);
            TEST_FAIL("warm pass missed nearly as much as the cold one");
        }
    }

    // ── Claim 2: a write kills the lines it touches ──────────────────────────
    // Sequence: write A, read it (now cached), overwrite with B, read again.
    // A stale cache serves A; a correct one re-fetches and serves B. Runs on
    // /home (the writable FAT data disk); skips gracefully if absent.
    const char *tail = NULL;
    vfs_filesystem_t *homefs = vfs_resolve_mount("/home/bctest.tmp", &tail);

    // The parent directory must actually EXIST on whatever filesystem won
    // the resolve: when no data disk is attached, "/home/..." falls through
    // to the root fs, which may have no /home at all — a boot shape, not a
    // bug (first seen on a bare FAT-root run, 2026-08-07). Probe it, so a
    // create failure BELOW stays a real tripwire (it caught the FF_FS_LOCK
    // table exhaustion the same day this probe was added).
    bool homeParentExists = false;
    if (homefs != NULL && homefs->fops->write != NULL)
    {
        char parent[64];
        int lastSlash = -1;
        int len = 0;
        while (tail[len] != '\0' && len < 62)
        {
            parent[len] = tail[len];
            if (tail[len] == '/')
                lastSlash = len;
            len++;
        }
        if (lastSlash <= 0)
            homeParentExists = true;   // tail lives at the mount's root
        else if (homefs->dops != NULL && homefs->dops->stat != NULL)
        {
            parent[lastSlash] = '\0';
            os64_dirent_t pe;
            homeParentExists =
                homefs->dops->stat(parent, &pe, homefs) == 0 &&
                (pe.flags & OS64_DE_DIR) != 0;
        }
    }

    if (homefs == NULL || homefs->fops->write == NULL || !homeParentExists ||
        !block_cache_covers(homefs->block_device_info))
    {
        kfree(chunk);
        if (!rootCovered)
        {
            // Cache is interposed somewhere, just under neither filesystem
            // this test can reach. Nothing exercised — say so, don't "pass".
            printd(DEBUG_TESTS, "\tSKIP: test_block_cache (cache active but covers neither the root nor /home)\n");
            return true;
        }
        printd(DEBUG_TESTS, "\tPASS: test_block_cache (hit rate only — no cached writable /home for the coherence half; warm hits %lu)\n",
               hitsWarm);
        return true;
    }

    static const char contentA[] = "the cache must never lie: A";
    static const char contentB[] = "the cache must never lie: B";
    vfs_file_t *w = NULL;

    if (homefs->fops->open(&w, tail, "w", homefs) != 0 || w == NULL)
    {
        kfree(chunk);
        TEST_FAIL("coherence: create failed");
    }
    w->fops->write(w, contentA, sizeof(contentA));
    w->fops->close(w);

    vfs_file_t *r = NULL;
    if (homefs->fops->open(&r, tail, "r", homefs) != 0 || r == NULL)
    {
        kfree(chunk);
        TEST_FAIL("coherence: first read-open failed");
    }
    r->fops->read(r, chunk, 256);
    r->fops->close(r);

    if (homefs->fops->open(&w, tail, "w", homefs) != 0 || w == NULL)
    {
        kfree(chunk);
        TEST_FAIL("coherence: rewrite-open failed");
    }
    w->fops->write(w, contentB, sizeof(contentB));
    w->fops->close(w);

    if (homefs->fops->open(&r, tail, "r", homefs) != 0 || r == NULL)
    {
        kfree(chunk);
        TEST_FAIL("coherence: second read-open failed");
    }
    int got = r->fops->read(r, chunk, 256);
    r->fops->close(r);
    if (homefs->fops->rm != NULL)
        homefs->fops->rm(tail, homefs);   // tidy; failure is not a verdict

    if (got < (int)sizeof(contentB) || memcmp(chunk, contentB, sizeof(contentB)) != 0)
    {
        kfree(chunk);
        TEST_FAIL("coherence: read-after-write returned STALE data — invalidation broken");
    }

    kfree(chunk);
    if (rootCovered)
        printd(DEBUG_TESTS, "\tPASS: test_block_cache (warm hits %lu, warm misses %lu vs cold %lu; write invalidation honest)\n",
               hitsWarm, missesWarm, missesCold);
    else
        printd(DEBUG_TESTS, "\tPASS: test_block_cache (coherence only — root device uncached; write invalidation honest)\n");
    return true;
}

// The dirent's new mtime field (2026-08-06, "cp raised its hand"): stat a
// binary the build just wrote and require its timestamp to land in the
// plausible present. The window is deliberately wide (2017..2100) but a
// packed-date conversion bug can't hit it — swap the month and day shifts,
// misplace FAT's 1980 base, forget the halved seconds' field width, and
// the result lands decades away. ext2's copy path is a straight epoch
// assignment; this test's real target is the FAT calendar math.
static bool test_dirent_mtime(void)
{
    if (kRootFilesystem == NULL || kRootFilesystem->dops == NULL ||
        kRootFilesystem->dops->stat == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_dirent_mtime (no root stat op)\n");
        return true;
    }

    os64_dirent_t e;
    memset(&e, 0xAA, sizeof(e));   // poison: a fill site that skips mtime shows up
    if (kRootFilesystem->dops->stat("/bin/top", &e, kRootFilesystem) != 0)
        TEST_FAIL("stat /bin/top failed");

    // 1,500,000,000 = mid-2017; 4,100,000,000 ≈ 2099. The build stamped
    // this file with the HOST's clock minutes-to-days ago.
    if (e.mtime < 1500000000ULL || e.mtime > 4100000000ULL)
    {
        printd(DEBUG_TESTS, "\tFAIL: test_dirent_mtime - mtime %lu is outside the plausible present\n",
               e.mtime);
        return false;
    }

    printd(DEBUG_TESTS, "\tPASS: test_dirent_mtime (/bin/top stamped %lu — the plausible present)\n",
           e.mtime);
    return true;
}

// The preemption backstop, end to end (2026-08-13; the 08-09 starvation debt's
// funeral). Pin a syscall-free ring-3 spinner (/tests/nosyscall) to an AP, watch that
// core's lease expiries tick up for half a second, then SIGKILL it — a signal
// that only a scheduler pass can deliver to a thread that never enters the
// kernel. Pre-backstop, both halves fail on tickless: the core takes zero
// passes once the hog lands, and the kill bit is never read. NOBACKSTOP
// boots (and periodic mode, and single-core) SKIP honestly.
static bool test_backstop_preemption(void)
{
    if (!kTicklessScheduler || !kSchedBackstopEnabled) {
        printd(DEBUG_TESTS, "\tSKIP: test_backstop_preemption (backstop not active this boot)\n");
        return true;
    }
    if (kMPCoreCount < 2) {
        printd(DEBUG_TESTS, "\tSKIP: test_backstop_preemption (needs an AP)\n");
        return true;
    }
    if (kRootFilesystem == NULL) {
        printd(DEBUG_TESTS, "\tSKIP: test_backstop_preemption (no root filesystem mounted)\n");
        return true;
    }

    uint32_t ap = kCPUInfo[1].apicID;
    uint64_t fires_before = kSchedBackstopFires[ap];

    // Ring 3 and PINNED: the exact starvation shape. An unpinned spinner
    // would still prove the lease, but the pin removes any doubt about WHOSE
    // counter must move.
    //
    // /tests/nosyscall, and the NAME is load-bearing: this test is only
    // meaningful against a thread that never enters the kernel, because a
    // thread that syscalls gets its scheduler pass for free and would prove
    // nothing about the backstop. It read "/bin/hog" until 2026-08-29, by
    // which time /bin/hog was the OTHER hog — the measuring instrument, which
    // reads the clock as it spins. Renaming the fixture after the property it
    // must have is what stops that recurring.
    task_t *hog = task_create("/tests/nosyscall", 0, NULL, kKernelTask, false, ap);
    if (hog == NULL) {
        printd(DEBUG_TESTS, "\tFAIL: test_backstop_preemption - task_create returned NULL\n");
        return false;
    }
    scheduler_submit_new_task(hog);

    // Let it burn for TEN LEASES — denominated in the LIVE knob
    // (kSchedBackstopMS: BACKSTOP=<ms> boots included), not in milliseconds,
    // so the window tracks exactly what it measures (Chris's catch,
    // 2026-08-13: a 500ms literal only held ten leases by coincidence of the
    // 50ms default; raise the lease and a literal window would fail a
    // healthy backstop). The assertion wants only 3 of the ~10, so a
    // wheezing nested-VM boot still passes while a dead backstop (0 fires)
    // still fails. wait() is ms-denominated and rounds UP to a whole tick,
    // so on a coarser-tick build this window can only stretch — which makes
    // the assertion safer, never tighter.
    for (int i = 0; i < 10; i++)
        wait(kSchedBackstopMS);

    uint64_t expiries = kSchedBackstopFires[ap] - fires_before;

    // Now the kill. No syscall will ever carry it home — only the pass the
    // next lease expiry forces. Forty leases of patience (2s at the 50ms
    // default), same denomination as above.
    task_signal_all_threads(hog, SIGKILL);
    for (int i = 0; i < 40 && !hog->exited; i++)
        wait(kSchedBackstopMS);

    // SINGLE EXIT from here down (the test_release discipline — see
    // test_elf_loader's note on the corpse protocol).
    bool ok = false;

    if (expiries < 3) {
        printd(DEBUG_TESTS, "\tFAIL: test_backstop_preemption - only %lu lease expiries on APIC %u in 500ms (wanted >=3)\n",
               expiries, ap);
    }
    else if (!hog->exited) {
        printd(DEBUG_TESTS, "\tFAIL: test_backstop_preemption - SIGKILL never reached the hog (no pass delivered it?)\n");
    }
    else {
        printd(DEBUG_TESTS, "\tPASS: test_backstop_preemption (%lu lease expiries on APIC %u, SIGKILL landed)\n",
               expiries, ap);
        ok = true;
    }

    test_release(hog);
    return ok;
}

// ─────────────────────────────────────────────────────────────────────────────

static void register_builtin_tests(void)
{
	test_register("kmalloc_not_null", test_kmalloc_not_null, TEST_PHASE_PREBOOT);
	test_register("fpu_state_round_trip", test_fpu_state_round_trip, TEST_PHASE_PREBOOT);
    test_register("page_fault_test_mode_returns", test_page_fault_does_not_panic_when_testing_flag_is_set, TEST_PHASE_PREBOOT);
    test_register("dlist_basic_operations", test_dlist_basic_operations, TEST_PHASE_PREBOOT);
    test_register("arena_create_and_destroy", test_arena_create_and_destroy, TEST_PHASE_PREBOOT);
    test_register("arena_basic_alloc", test_arena_basic_alloc, TEST_PHASE_PREBOOT);
    test_register("arena_aligned_alloc", test_arena_aligned_alloc, TEST_PHASE_PREBOOT);
    test_register("arena_reset", test_arena_reset, TEST_PHASE_PREBOOT);
    test_register("arena_growth", test_arena_growth, TEST_PHASE_PREBOOT);
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
    test_register("dynamic_linking", test_dynamic_linking, TEST_PHASE_POSTBOOT);
    test_register("task_args", test_task_args, TEST_PHASE_POSTBOOT);
    test_register("env_growth", test_env_growth, TEST_PHASE_POSTBOOT);
    // LATE, both of them, and they are the two that bought the phase: on a
    // machine with a NIC these were twenty of the ~thirty seconds between
    // power-on and a prompt, and neither answers a question anyone needs
    // answered before the shell exists. task_teardown_leak spends five
    // seconds proving the undertaker has gone quiet before it dares measure;
    // net_tcp_refused spends a ten-second dial timeout whenever the gateway
    // drops the SYN instead of resetting it, and then SKIPS. Off the critical
    // path, both costs stop being costs at all.
    test_register("task_teardown_leak", test_task_teardown_leak, TEST_PHASE_LATE);
    test_register("ext2_real_partition", test_ext2_real_partition, TEST_PHASE_POSTBOOT);
    test_register("mount_table", test_mount_table, TEST_PHASE_POSTBOOT);
    test_register("devfs", test_devfs, TEST_PHASE_POSTBOOT);
    test_register("net_arp_pending_order", test_net_arp_pending_order, TEST_PHASE_POSTBOOT);
    test_register("net_wire", test_net_wire, TEST_PHASE_POSTBOOT);
    test_register("net_arp", test_net_arp, TEST_PHASE_POSTBOOT);
    test_register("net_ping", test_net_ping, TEST_PHASE_POSTBOOT);
    test_register("net_echo_responder", test_net_echo_responder, TEST_PHASE_POSTBOOT);
    test_register("net_dhcp", test_net_dhcp, TEST_PHASE_POSTBOOT);
    test_register("net_udp_conn", test_net_udp_conn, TEST_PHASE_POSTBOOT);
    test_register("net_icmp_conn", test_net_icmp_conn, TEST_PHASE_POSTBOOT);
    test_register("net_tcp_refused", test_net_tcp_refused, TEST_PHASE_LATE);
    // The write gauntlets carry TEST_POLICY_RO: a failure here impeaches the
    // WRITE path while logd is actively appending to a disk — continuing to
    // write compounds the damage, halting costs the analysis. Demote every
    // mount to read-only and keep the lights on (the policy taxonomy lives
    // in test_framework.h; the demotion engine in vfs.c).
    test_register_policy("vfs_write_mkdir", test_vfs_write_mkdir, TEST_PHASE_POSTBOOT, TEST_POLICY_RO);
    test_register_policy("vfs_rename", test_vfs_rename, TEST_PHASE_POSTBOOT, TEST_POLICY_RO);
    test_register_policy("ext2_orphan", test_ext2_orphan, TEST_PHASE_POSTBOOT, TEST_POLICY_RO);
    test_register_policy("ext2_readonly_demotion", test_ext2_readonly_demotion, TEST_PHASE_POSTBOOT, TEST_POLICY_RO);
    test_register_policy("ext2_secondary_write", test_ext2_secondary_write, TEST_PHASE_POSTBOOT, TEST_POLICY_RO);
    // Writes, but NOT a write gauntlet: what it impeaches on failure is the
    // shared-object registry, not the disk, so demoting every mount to
    // read-only would be an answer to a question this test never asked. It
    // runs after the gauntlets above, which is what makes its own writes
    // trustworthy.
    test_register("shared_object_reload", test_shared_object_reload, TEST_PHASE_POSTBOOT);
    test_register("console_read_deadline", test_console_read_deadline, TEST_PHASE_POSTBOOT);
    test_register_policy("block_cache", test_block_cache, TEST_PHASE_POSTBOOT, TEST_POLICY_RO);
    test_register("dirent_mtime", test_dirent_mtime, TEST_PHASE_POSTBOOT);
    test_register("backstop_preemption", test_backstop_preemption, TEST_PHASE_POSTBOOT);
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
    size_t failedPanic = 0;   // failures at TEST_POLICY_PANIC — halt after the phase
    size_t failedRO = 0;      // failures at TEST_POLICY_RO — demote mounts after the phase

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

        // THE DRILL FLAG IS CLEARED HERE, BY THE RUNNER, ON PURPOSE (2026-08-20).
        // A fault-injection test raises it before inducing an alarm (vfs.h says
        // what it does), and those tests are full of early `return false` paths
        // — hand-clearing it at each one is exactly the kind of bookkeeping that
        // stays right for a month and then silently swallows a REAL demotion
        // warning for the rest of the project's life. The runner owns the reset
        // so no test can leak it, and a test that forgets to clear it is simply
        // not a bug that can exist.
        kTestingExpectedNoise = false;

        // The TESTS= cmdline knob (kernel_commandline.c) overrides every
        // test's registered policy for this boot: "panic" restores the old
        // halt-on-any-failure strictness, "warn" forces continue-always
        // (bare-metal triage). Anything else (or unset) honors the
        // registrations.
        test_policy_t policy = test->policy;
        if (strncmp(kTestsPolicyOverride, "panic", 6) == 0)
            policy = TEST_POLICY_PANIC;
        else if (strncmp(kTestsPolicyOverride, "warn", 5) == 0)
            policy = TEST_POLICY_WARN;

        if (result) {
            ++passed;
            printd(DEBUG_TESTS, "\t[Test] %s... OK\n", name);
        } else {
            ++failed;
            if (policy == TEST_POLICY_PANIC)
                ++failedPanic;
            else if (policy == TEST_POLICY_RO)
                ++failedRO;
            printd(DEBUG_TESTS, "\t[Test] %s... FAIL\n", name);
            // PERMANENT screen print, not just serial: on a machine with no
            // COM port (the P5), a failure whose name only went to serial is
            // a confession sealed in an envelope — and under WARN policy the
            // glass line is the whole verdict, so it must carry the policy too.
            printf("  FAIL: %s%s\n", name,
                   policy == TEST_POLICY_PANIC ? " [halting]" :
                   policy == TEST_POLICY_RO    ? " [demoting mounts to read-only]" :
                                                 " [continuing]");
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

    // The verdicts, severest first (test_framework.h owns the taxonomy —
    // ext2's s_errors trio reborn, ratified 2026-08-08: "a failed test means
    // I want to do analysis, not stare at a panic"):
    if (failedPanic > 0) {
        // panic(), not a bare cli/hlt: panic force-drains the log buffer to
        // serial before halting.  The old halt stranded this message — and any
        // test results logd hadn't drained yet — in the ring buffer forever.
        panic("Test framework: %u %s test(s) failed at PANIC severity. System halted.\n",
              (unsigned int)failedPanic, label);
    }
    if (failedRO > 0)
        vfs_demote_all_mounts_readonly("write-path test failure");
    // WARN-only failures: already named on the glass above; the boot goes on.
}

void test_run_preboot(void)
{
    test_run_phase(TEST_PHASE_PREBOOT, "pre-boot");
}

void test_run_postboot(void)
{
    test_run_phase(TEST_PHASE_POSTBOOT, "post-boot");
}

// The LATE phase, on its own kernel thread (kernel.c creates it as
// "/latetests" once the shells are seated). The boot does not wait for this:
// by the time it runs there is a prompt on the glass, which is the entire
// point — see TEST_PHASE_LATE's comment in test_framework.h.
//
// It ends by exiting the thread rather than returning, because a kernel
// thread's "return" has nowhere to go: the task's entry RIP was set straight
// to this function, so there is no caller frame beneath it.
void late_tests_thread(void)
{
    test_run_phase(TEST_PHASE_LATE, "late");
    task_exit();
}
