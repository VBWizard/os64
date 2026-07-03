#include <stddef.h>

#include "panic.h"
#include "BasicRenderer.h"
#include "serial_logging.h"
#include "smp_core.h"
#include "task.h"
#include "CONFIG.h"
#include "log.h"
#include "memory/paging.h"
#include "memory/memcpy.h"
#include "exceptions.h"
#include "memory/vma.h"
#include "kmalloc.h"
#include "allocator.h"
#include "shared_object.h"

uint64_t gLastFaultRbp = 0;
uint64_t gLastFaultRsp = 0;
uint64_t gLastFaultErrorCode = 0;
bool kTestingPageFaults = false;
uint64_t kTestingPageFaultResumeRip = 0;
volatile uint64_t kPageFaultCount;

static bool is_canonical_address(uint64_t address)
{
	uint64_t upper = address >> 47;
	return upper == 0 || upper == 0x1FFFF;
}

static bool address_is_mapped(uint64_t address)
{
	uintptr_t pte = paging_walk_paging_table_keep_flags((pt_entry_t*)kKernelPML4v, address, true);
	if (pte == 0xbadbadba) {
		return false;
	}
	return (pte & PAGE_PRESENT) != 0;
}

//NOTE: Won't work with userland RIPs. Will need to modify to accept the CR3 for non-kernel processes once we have a userland
void dump_stack_trace(uint64_t rip)
{
	printf("Stack trace (most recent call first):\n");
	printf("  [0] RIP=0x%016lx\n", rip);
	printf("  Captured RSP=0x%016lx RBP=0x%016lx\n", gLastFaultRsp, gLastFaultRbp);

	if (gLastFaultErrorCode & (1ull << 2)) {
		printf("  <fault originated from user mode>\n");
	}

	uint64_t rbp = gLastFaultRbp;
	if (rbp == 0) {
		printf("  <no frame pointer captured>\n");
		return;
	}

	if (!is_canonical_address(rbp)) {
		printf("  <frame pointer 0x%016lx non-canonical>\n", rbp);
		return;
	}

	if (!address_is_mapped(rbp) || !address_is_mapped(rbp + sizeof(uint64_t))) {
		printf("  <frame pointer 0x%016lx unmapped>\n", rbp);
		return;
	}

	const uint32_t max_frames = 16;
	for (uint32_t frame = 1; frame < max_frames; frame++) {
		uint64_t *frame_ptr = (uint64_t*)rbp;
		uint64_t next_rbp = frame_ptr[0];
		uint64_t return_address = frame_ptr[1];

		if (!is_canonical_address(return_address)) {
			printf("  [%u] <non-canonical return address 0x%016lx>\n", frame, return_address);
			break;
		}

		printf("  [%u] RIP=0x%016lx\n", frame, return_address);

		if (next_rbp == 0) {
			break;
		}
		if (next_rbp <= rbp) {
			printf("  <next frame pointer 0x%016lx not higher than current 0x%016lx>\n", next_rbp, rbp);
			break;
		}
		if (!is_canonical_address(next_rbp)) {
			printf("  <next frame pointer 0x%016lx non-canonical>\n", next_rbp);
			break;
		}
		if (!address_is_mapped(next_rbp) || !address_is_mapped(next_rbp + sizeof(uint64_t))) {
			printf("  <next frame pointer 0x%016lx unmapped>\n", next_rbp);
			break;
		}

		rbp = next_rbp;
	}
}

void exception_panic(const char* message, uint64_t rip, uint64_t error_code) {
    core_local_storage_t* core = get_core_local_storage();

    printf("\n>>> EXCEPTION PANIC: %s <<<                      \n", message);  // 🛠 FIXED: Actually print the message!
    printf(">>> AP %lu (Thread 0x%08x) <<<                        \n", core->apic_id, core->threadID);
    printf(">>> Faulting instruction: 0x%016lx <<<             \n", rip);
    
    if (error_code != 0xFFFFFFFFFFFFFFFF) {
        printf(">>> Error Code: 0x%lx <<<                          \n", error_code);
    }
    if (core->currentThread) {
		task_t *task = (task_t*)core->currentThread->ownerTask;

        printf(">>> Excepting Task: %s <<<                         \n", task->path);
    } else {
        printf(">>> No current task (core likely idle) <<<         \n");
    }

	// **Log it only if logging is initialized**
	if (kLoggingInitialized) {
		printd(DEBUG_EXCEPTIONS, "EXCEPTION: %s (AP %lu, Thread %lu, RIP: 0x%016lx, Error Code: 0x%lx)\n",
				message, core->apic_id, core->threadID, rip, error_code);
	}

	while (1) { __asm__ volatile ("cli\nhlt\n"); }
}

void handle_divide_by_zero(uint64_t rip) {
    exception_panic("Divide by zero (#DE) occurred!", rip, 0xFFFFFFFFFFFFFFFF);
}

void handle_invalid_opcode(uint64_t rip) {
    exception_panic("Invalid opcode (#UD) occurred!", rip, 0xFFFFFFFFFFFFFFFF);
}

void handle_double_fault(uint64_t rip) {
    exception_panic("Double fault (#DF) occurred!", rip, 0xFFFFFFFFFFFFFFFF);
}

void handle_general_protection_fault(uint64_t error_code, uint64_t rip) {
    exception_panic("General Protection Fault (#GP) occurred!", rip, error_code);
}


static void log_page_fault_bits(uint64_t error_code)
{
	const struct {
		uint64_t mask;
		const char *label;
	} bit_info[] = {
		{1ull << 0, "Present (bit 0)"},
		{1ull << 1, "Write (bit 1)"},
		{1ull << 2, "User (bit 2)"},
		{1ull << 3, "Reserved bit violation (bit 3)"},
		{1ull << 4, "Instruction fetch (bit 4)"},
	};

	bool any = false;
	for (size_t i = 0; i < sizeof(bit_info) / sizeof(bit_info[0]); i++) {
		if (error_code & bit_info[i].mask) {
			printf("  %s\n", bit_info[i].label);
			any = true;
		}
	}

	if (!any) {
		printf("  No recognized fault bits set.\n");
	}
}

void handle_page_fault(uint64_t cr2, uint64_t error_code, uint64_t rip)
{
    if (kTestingPageFaults)
    {
        if (gLastFaultRsp != 0 && kTestingPageFaultResumeRip != 0)
        {
            uint64_t *stack = (uint64_t *)gLastFaultRsp;
            // Error code at stack[0], return RIP at stack[1]
            stack[1] = kTestingPageFaultResumeRip;
        }
        printd(DEBUG_EXCEPTIONS, "\tPage fault occurred during test mode, returning without halt.\n");
        // Clear the CR2
        __asm__ __volatile__(
            "xor rax, rax\n\t"
            "mov cr2, rax\n\t");
        return;
    }

    printd(DEBUG_EXCEPTIONS, "PAGE FAULT at RIP=0x%016lx, CR2=0x%016lx, ERROR=0x%lx\n", rip, cr2, error_code);
    task_t *task = get_core_local_storage()->task;
    vma_t *vma = vma_lookup(task, cr2);
    if (!vma)
    {
        printd(DEBUG_EXCEPTIONS, "No VMA found for address 0x%016lx.\n", cr2);
        log_page_fault_bits(error_code);
        dump_stack_trace(rip);
        panic("Paging exception: Invalid memory access with no VMA");
    }

    printd(DEBUG_EXCEPTIONS, "Found VMA: 0x%016lx - 0x%016lx (prot=0x%x, cow=%d)\n", vma->start, vma->end, vma->prot, vma->cow);

    // Calculate aligned fault address
    uintptr_t aligned = cr2 & ~(PAGE_SIZE - 1);

    // Classify the fault from the error code:
    //   bit 0 (P): 0 = page not present, 1 = page present (protection violation)
    //   bit 1 (W): 0 = read fault,        1 = write fault
    bool page_was_present = (error_code & 0x1) != 0;
    bool was_write        = (error_code & 0x2) != 0;

    if (page_was_present && was_write && vma->cow)
    {
        // Copy-on-Write fault: the page is present but mapped read-only because
        // it is (or was) shared with another task.  Allocate a private copy,
        // duplicate the content, then remap writable so the faulting store can retry.
        uintptr_t old_phys = paging_walk_paging_table((pt_entry_t *)task->pml4v, aligned);
        if (!old_phys || old_phys == 0xbadbadba)
            panic("CoW fault: page table walk did not find the original page");

        // kmalloc_aligned guarantees the page is accessible via HHDM in kKernelPML4.
        // allocate_memory_aligned() does not make that guarantee.
        void *new_virt = kmalloc_aligned(PAGE_SIZE);
        if (!new_virt)
            panic("CoW fault: failed to allocate replacement page");
        uintptr_t new_phys = (uintptr_t)new_virt - kHHDMOffset;

        // Copy the old page's content via HHDM — the source physical page is
        // accessible at (old_phys | kHHDMOffset); the dest is new_virt directly.
        memcpy(new_virt,
               (void *)(old_phys | kHHDMOffset),
               PAGE_SIZE);

        // Remap the virtual address to the new private page, now writable.
        uint64_t cow_flags = PAGE_PRESENT | PAGE_USER | PAGE_WRITE;
        paging_map_page((pt_entry_t *)task->pml4v, aligned, new_phys, cow_flags);

        // paging_map_page does not flush the TLB on map (only on unmap), so we
        // must invalidate this entry explicitly or the CPU retries against the
        // stale read-only TLB entry and faults again immediately.
        __asm__ volatile("invlpg [%0]" :: "r"(aligned) : "memory");

        printd(DEBUG_EXCEPTIONS, "CoW: 0x%016lx privatised (old phys 0x%016lx -> new phys 0x%016lx)\n",
               aligned, old_phys, new_phys);
        kPageFaultCount++;
        return;
    }

    if (page_was_present)
    {
        // Page is present but the access was denied and this VMA is not CoW.
        // This is a genuine protection violation, not a recoverable fault.
        printd(DEBUG_EXCEPTIONS, "Protection violation at RIP=0x%016lx, CR2=0x%016lx\n", rip, cr2);
        log_page_fault_bits(error_code);
        dump_stack_trace(rip);
        panic("Paging exception: protection violation (write to read-only non-CoW page)");
    }

    // Demand page fault: page is not present yet.
    if (vma->flags & MAP_SHARED_LIBRARY)
    {
        // This VMA belongs to a dynamically-linked image (library or main
        // executable) — vma->file is a shared_object_t*, not a vfs_file_t*.
        // Resolution goes through the per-image page cache instead of a
        // plain per-VMA file read: whichever task touches a given page
        // first reads it from the file and applies that page's
        // relocations; every task after gets the same physical page. The
        // symbols those relocations reference resolve against the OBJECT'S
        // own dependency scope (so + so->deps, inside shared_object.c) —
        // deliberately not this task's view, since the resolved page is
        // cached and shared with every other task that maps this object.
        // Never PAGE_WRITE here even for a writable segment — vma->cow
        // (checked above) governs the write path separately, through the
        // existing, unmodified CoW branch.
        shared_object_t *so = (shared_object_t *)vma->file;
        size_t page_idx = (aligned - so->load_bias) / PAGE_SIZE;

        uintptr_t phys = shared_object_resolve_page(so, page_idx);
        if (!phys)
            panic("Failed to resolve shared-object page during fault resolution");

        paging_map_page((pt_entry_t *)task->pml4v, aligned, phys, PAGE_PRESENT | PAGE_USER);
        kPageFaultCount++;
        return;
    }

    // Ordinary demand page fault: page is not present yet.  Resolve via the
    // VMA's backing (static executables, anonymous memory).
    uintptr_t phys = vma_resolve_backing_page(vma, cr2);
    if (!phys)
        panic("Failed to resolve page during fault resolution");

    // Map the page into task's address space
    uint64_t flags = PAGE_PRESENT | PAGE_USER;
    if (vma->prot & PROT_WRITE)
        flags |= PAGE_WRITE;

    paging_map_page((pt_entry_t *)task->pml4v, aligned, phys, flags);

    printd(DEBUG_EXCEPTIONS, "Mapped page at 0x%016lx with flags 0x%lx\n", aligned, flags);

    kPageFaultCount++;
}


void handle_machine_check(uint64_t rip) {
    exception_panic("Machine Check (#MC) occurred!", rip, 0xFFFFFFFFFFFFFFFF);
}
