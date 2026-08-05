#include <stddef.h>

#include "panic.h"
#include "BasicRenderer.h"
#include "serial_logging.h"
#include "sprintf.h"
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

// Emit a line to BOTH the screen and the serial port.  Exceptions must be
// diagnosable from either — a #GP that only ever appears on the framebuffer
// is invisible to headless/CI runs.  Serial output goes DIRECTLY through
// serial_print_string, bypassing the printd ring buffer entirely: this core
// is about to cli/hlt, so anything left in the buffer depends on another
// core's logd/kworker still being alive to drain it (and logd_thread(false)
// is only a try-lock — it can silently drain nothing).  A panic path must
// not have dependencies.
#define EXCEPTION_PRINT(fmt, ...) do { \
        printf(fmt, ##__VA_ARGS__); \
        char _exc_line[512]; \
        snprintf(_exc_line, sizeof(_exc_line), fmt, ##__VA_ARGS__); \
        serial_print_string(_exc_line); \
    } while (0)

void exception_panic(const char* message, uint64_t rip, uint64_t error_code) {
    core_local_storage_t* core = get_core_local_storage();

    EXCEPTION_PRINT("\n>>> EXCEPTION PANIC: %s <<<                      \n", message);
    EXCEPTION_PRINT(">>> AP %lu (Thread 0x%08x) <<<                        \n", core->apic_id, core->threadID);
    EXCEPTION_PRINT(">>> Faulting instruction: 0x%016lx <<<             \n", rip);

    if (error_code != 0xFFFFFFFFFFFFFFFF) {
        EXCEPTION_PRINT(">>> Error Code: 0x%lx <<<                          \n", error_code);
    }
    if (core->currentThread) {
		task_t *task = (task_t*)core->currentThread->ownerTask;

        EXCEPTION_PRINT(">>> Excepting Task: %s <<<                         \n", task->path);
    } else {
        EXCEPTION_PRINT(">>> No current task (core likely idle) <<<         \n");
    }

	// Best-effort drain of the printd ring buffer too, so the log context
	// LEADING UP to the exception makes it out with us.  Try-lock inside —
	// if another core holds the drain lock this does nothing, which is why
	// the exception report itself went directly to serial above.
	if (kLoggingInitialized) {
		logd_thread(false);
	}

	while (1) { __asm__ volatile ("cli\nhlt\n"); }
}

void handle_divide_by_zero(uint64_t rip) {
    exception_panic("Divide by zero (#DE) occurred!", rip, 0xFFFFFFFFFFFFFFFF);
}

void handle_invalid_opcode(uint64_t rip) {
    exception_panic("Invalid opcode (#UD) occurred!", rip, 0xFFFFFFFFFFFFFFFF);
}

// A double fault means the CPU could not deliver some FIRST exception —
// nearly always because the stack it had to push onto was unusable. So the
// interesting evidence is not "a #DF happened", it is WHAT RSP and RFLAGS
// were when it did: an RSP outside any stack region, or RFLAGS carrying
// bits no kernel thread should have (TF, NT, IOPL≠0, AC), says the thread
// was dispatched with a corrupt register frame rather than that it did
// something wrong.
void handle_double_fault_frame(uint64_t rip, uint64_t rsp, uint64_t rflags)
{
	core_local_storage_t *core = get_core_local_storage();
	printf("\n>>> DOUBLE FAULT on AP %lu <<<\n", core ? core->apic_id : 0);
	printf(">>> RIP=0x%016lx RSP=0x%016lx RFLAGS=0x%016lx <<<\n", rip, rsp, rflags);
	// Name the usual suspects rather than making a reader decode bits.
	if (rflags & 0x100)   printf(">>>   RFLAGS.TF set — single-step on a kernel thread <<<\n");
	if (rflags & 0x4000)  printf(">>>   RFLAGS.NT set <<<\n");
	if (rflags & 0x3000)  printf(">>>   RFLAGS.IOPL != 0 <<<\n");
	if (rflags & 0x40000) printf(">>>   RFLAGS.AC set <<<\n");
	exception_panic("Double Fault (#DF) — the first fault could not be delivered", rip, 0);
}

void handle_double_fault(uint64_t rip) {
    exception_panic("Double fault (#DF) occurred!", rip, 0xFFFFFFFFFFFFFFFF);
}

void handle_general_protection_fault(uint64_t error_code, uint64_t rip) {
    exception_panic("General Protection Fault (#GP) occurred!", rip, error_code);
}

// Every CPU exception that had no gate until 2026-08-01. They share one
// handler because the point isn't to RECOVER from them — it's to say which
// one happened. An unpopulated gate makes the CPU raise #GP instead, whose
// error code names the IDT slot it couldn't deliver; that is how a debug
// exception spent an afternoon impersonating a protection fault in the
// idle loop. A named panic costs nothing and answers the first question.
static const char *exception_name(uint64_t vector)
{
    switch (vector)
    {
        case 1:  return "Debug Exception (#DB) — single-step or breakpoint";
        case 2:  return "Non-Maskable Interrupt (NMI)";
        case 3:  return "Breakpoint (#BP) — an int3 executed";
        case 4:  return "Overflow (#OF)";
        case 5:  return "BOUND Range Exceeded (#BR)";
        case 7:  return "Device Not Available (#NM) — FPU/SSE used before CR0 setup";
        case 10: return "Invalid TSS (#TS)";
        case 11: return "Segment Not Present (#NP)";
        case 12: return "Stack-Segment Fault (#SS)";
        case 16: return "x87 Floating-Point Error (#MF)";
        case 17: return "Alignment Check (#AC)";
        case 19: return "SIMD Floating-Point Exception (#XM)";
        default: return "Unexpected CPU exception";
    }
}

void handle_unexpected_exception(uint64_t vector, uint64_t error_code, uint64_t rip)
{
    // The vector number goes in the banner too: a reader who doesn't have
    // the Intel manual open still gets something greppable.
    char msg[160];
    snprintf(msg, sizeof(msg), "%s [vector %lu]", exception_name(vector), vector);
    exception_panic(msg, rip, error_code);
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

// A user-mode fault the demand pager can't resolve is the APP's bug, not the
// kernel's: kill the task, keep the OS.  This is the segmentation fault, and
// the exit code is 139 by the oldest convention in Unix — 128 + 11, signal 11
// being SIGSEGV's number since the Seventh Edition signal table (os64 doesn't
// deliver signals for this yet, but the exit code keeps the lineage so shell
// scripts and muscle memory read it correctly).
//
// Safe from #PF context: interrupts are already masked (the stub cli'd), we
// are on the CPU-switched kernel interrupt stack, and task_exit() is built
// for exactly this situation — it re-points RSP at that stack's top, switches
// to kKernelPML4, and schedules away, never returning to the faulting frame.
static void __attribute__((noreturn)) user_fault_kill(task_t *task, const char *why,
    uint64_t cr2, uint64_t error_code, uint64_t rip)
{
	printf("\nSegmentation fault: task %lu, %s at 0x%016lx (RIP=0x%016lx, error=0x%lx)\n",
	       task->taskID, why, cr2, rip, error_code);
	printd(DEBUG_EXCEPTIONS, "Segmentation fault: task %lu, %s CR2=0x%016lx RIP=0x%016lx error=0x%lx\n",
	       task->taskID, why, cr2, rip, error_code);

	task->retVal = 139;   // 128 + SIGSEGV(11)
	task_exit();
	__builtin_unreachable();
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

    // DEBUG_DEMAND_PAGING, not EXCEPTIONS: most faults that reach this line
    // are the demand pager being asked to do its job, and EXCEPTIONS is
    // always-on — routine paging traffic was drowning the default log. Every
    // FATAL path below re-announces RIP/CR2/error on DEBUG_EXCEPTIONS (plus
    // decoded bits and a stack trace) before it panics, so demoting this
    // line loses nothing when a fault is actually news.
    printd(DEBUG_DEMAND_PAGING, "PAGE FAULT at RIP=0x%016lx, CR2=0x%016lx, ERROR=0x%lx\n", rip, cr2, error_code);

    // Guard against faults BEFORE per-core state exists (early boot: CLS not
    // allocated and/or GS base not programmed). Without this, [gs:0] returns
    // junk, ->task is junk, and vma_lookup faults on the junk pointer — the
    // handler then re-enters itself until the stack dies in a triple fault,
    // taking the diagnosable panic below with it.
    task_t *task = kCoreLocalStorage ? get_core_local_storage()->task : NULL;
    if (!task)
    {
        log_page_fault_bits(error_code);
        dump_stack_trace(rip);
        panic("Page fault with no task context (early boot?): RIP=0x%016lx, CR2=0x%016lx, ERROR=0x%lx",
              rip, cr2, error_code);
    }
    vma_t *vma = vma_lookup(task, cr2);
    if (!vma)
    {
        // Error bit 2 = the faulting access came from ring 3: the app chased
        // a wild pointer.  Its problem, not ours — segfault the task.  (The
        // kernel-mode paths below stay panics: a ring-0 no-VMA fault is a
        // kernel bug, and the syscall copy helpers pre-validate user ranges
        // precisely so a bad user pointer can never fault down here in ring 0.)
        if (error_code & 0x4)
            user_fault_kill(task, "access to unmapped address", cr2, error_code, rip);
        printd(DEBUG_EXCEPTIONS, "No VMA found for address 0x%016lx.\n", cr2);
        log_page_fault_bits(error_code);
        dump_stack_trace(rip);
        // A fault in the HHDM range is the lazy-HHDM tripwire firing (see
        // paging.h): physical memory is only HHDM-mapped while allocated, so
        // this is a use-after-free, a wild physical-address dereference, or
        // memory that never came from the allocator (e.g. MMIO that needs an
        // explicit mapping). Say so, rather than the generic no-VMA message.
        if (kHHDMMaintenanceEnabled && cr2 >= kHHDMOffset && cr2 < kHHDMOffset + 0x1000000000000UL)
            panic("Paging exception: HHDM access to unallocated physical address 0x%016lx — use-after-free or wild pointer?", cr2 - kHHDMOffset);
        panic("Paging exception: Invalid memory access with no VMA\n");
    }

    // Per-fault detail: rides DETAILED so the base demand-paging channel
    // stays a readable two-lines-per-fault (announce + resolution).
    printd(DEBUG_DEMAND_PAGING | DEBUG_DETAILED, "Found VMA: 0x%016lx - 0x%016lx (prot=0x%x, cow=%d)\n", vma->start, vma->end, vma->prot, vma->cow);

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

        printd(DEBUG_DEMAND_PAGING, "CoW: 0x%016lx privatised (old phys 0x%016lx -> new phys 0x%016lx)\n",
               aligned, old_phys, new_phys);
        kPageFaultCount++;
        return;
    }

    if (page_was_present)
    {
        // Ring-3 protection violation on a non-CoW page (write to read-only
        // data, jump into no-exec, etc.): the app's bug — segfault the task.
        // Ring-0 violations fall through to the diagnosing panic below.
        if (error_code & 0x4)
            user_fault_kill(task, "protection violation", cr2, error_code, rip);

        // Page is present but the access was denied and this VMA is not CoW.
        // This is a genuine protection violation, not a recoverable fault.
        // Decode the error bits into the panic message rather than assuming
        // write-to-read-only: a USER instruction fetch through intermediate
        // tables lacking PAGE_USER lands here too (that exact fault, error
        // 0x15, is how ring-3 bring-up found the paging_map_page U/S bug),
        // and a wrong message sends the reader hunting in the wrong place.
        printd(DEBUG_EXCEPTIONS, "Protection violation at RIP=0x%016lx, CR2=0x%016lx\n", rip, cr2);
        log_page_fault_bits(error_code);
        dump_stack_trace(rip);
        panic("Paging exception: protection violation (%s access, %s mode, error=0x%lx) on a present non-CoW page",
              (error_code & 0x10) ? "instruction-fetch" : ((error_code & 0x2) ? "write" : "read"),
              (error_code & 0x4) ? "user" : "supervisor",
              error_code);
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

    printd(DEBUG_DEMAND_PAGING, "Mapped page at 0x%016lx with flags 0x%lx\n", aligned, flags);

    kPageFaultCount++;
}


void handle_machine_check(uint64_t rip) {
    exception_panic("Machine Check (#MC) occurred!", rip, 0xFFFFFFFFFFFFFFFF);
}
