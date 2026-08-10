#include "CONFIG.h"
#include <stdbool.h>
#include "thread.h"
#include "kmalloc.h"
#include "allocator.h"
#include "paging.h"
#include "BasicRenderer.h"
#include "serial_logging.h"
#include "memset.h"
#include "gdt.h"
#include "task.h"
#include "panic.h"

extern uintptr_t kKernelBaseAddressV;
extern uintptr_t kKernelBaseAddressP;
extern uintptr_t kHHDMOffset;
extern uintptr_t kKernelPML4v;
extern pt_entry_t kKernelPML4;

//Will be a BSS variable so all indices will be 0 when the kernel is loaded
uint64_t kTIDBitmap[MAX_THREADS / sizeof(uint64_t) * 8];
uint32_t kFirstAvailableThreadID = RESERVED_THREADS;

/// @brief Attempts to set a bitmap bit to indicate that we are marking that index as "used".
/// @param tid The thread ID to mark as used
/// @return True if the bit was successfully set, false if it was already 1 and couldn't be set
bool mark_TID_used(uint32_t tid)
{
    uint32_t index = tid / 64;
    uint32_t bit   = tid % 64;
    uint64_t *pword = &kTIDBitmap[index];
    bool wasSet=false;

    /*
       lock bts [pword], bit
         - Sets the carry flag (CF) to the OLD value of the bit.
         - Sets the bit to 1.
       sbb wasSet, wasSet
         - Moves CF into wasSet (wasSet becomes 1 if bit was already set, else 0).
    */
    asm volatile(
        "lock bts qword ptr [%1], %2       \n"
        "sbb %0, %0                        \n"
        : "=r"(wasSet),      // %0 (output)
          "+m"(*pword)       // %1 (input+output: the memory location)
        : "r"((uint64_t)bit) // %2 (input: the bit index)
        : "memory", "cc"
    );

    return (!wasSet);
}

bool mark_TID_unused(uint32_t tid)
{
    uint32_t index = tid / 64;
    uint32_t bit   = tid % 64;
    uint64_t *pword = &kTIDBitmap[index];
    bool wasSet=false;

    /*
       lock bts [pword], bit
         - Sets the carry flag (CF) to the OLD value of the bit.
         - Sets the bit to 1.
       sbb wasSet, wasSet
         - Moves CF into wasSet (wasSet becomes 1 if bit was already set, else 0).
    */
    asm volatile(
        "lock btr qword ptr [%1], %2       \n"
        "sbb %0, %0                        \n"
        : "=r"(wasSet),      // %0 (output)
          "+m"(*pword)       // %1 (input+output: the memory location)
        : "r"((uint64_t)bit) // %2 (input: the bit index)
        : "memory", "cc"
    );

	return wasSet;
}

/// @brief Create and return a block of aligned, guarded stack memory
/// @param pml4 CR3 for the thread
/// @param virtualStart The virtual address to map the stack to.  This value is populated by this function, on return
/// @param requestedLength The length of the stack to be created
/// @param isRing3Stack Is this a user stack (for including PAGE_USER in the mapping flags)
/// @return The physical start address of the block of unmapped memory preceding the stack
uintptr_t thread_allocate_guarded_stack_memory(uintptr_t pml4, uintptr_t *virtualStart, uint64_t requestedLength, bool isRing3Stack)
{
	//Create and return a block of aligned, guarded stack memory
	//To do so, allocate more memory than we need and leave the first Xk and last Xk unmapped (paging)
	//Allocate THREAD_STACK_GUARD_PAGE_COUNT * 2 so there is room for THREAD_STACK_GUARD_PAGE_COUNT pages on each side of the stack
	uint64_t physStackAddress = allocate_memory_aligned(requestedLength + (THREAD_STACK_GUARD_PAGE_COUNT * PAGE_SIZE * 2));
	if (!physStackAddress) {
    	panic("Failed to allocate stack memory!\n");
	}
    //If *no* starting virtual address, then calculate on in HHMD
    if (*virtualStart==0)
    {
    	*virtualStart = physStackAddress | kHHDMOffset;//isRing3Stack?THREAD_USER_STACK_INITIAL_VIRT_ADDRESS:THREAD_KERNEL_STACK_VIRTUAL_START;
    	//				(physStackAddress + (PAGE_SIZE*THREAD_STACK_GUARD_PAGE_COUNT)) | kHHDMOffset;
    }
	uint64_t flags = PAGE_PRESENT | PAGE_WRITE;
	if (isRing3Stack)
		flags |= PAGE_USER;

	uint64_t pagesToMap = requestedLength / PAGE_SIZE;
	if (requestedLength % PAGE_SIZE)
		pagesToMap++;
	uint64_t physStartMapAddress = physStackAddress + (PAGE_SIZE*THREAD_STACK_GUARD_PAGE_COUNT);
	paging_map_pages((pt_entry_t*)pml4, *virtualStart, physStartMapAddress, pagesToMap, flags);

	return physStackAddress;
}

uint64_t get_thread_id()
{
	uint32_t tid;
	do
	{
		tid = (kFirstAvailableThreadID++)%MAX_THREADS;
		if (!kFirstAvailableThreadID)
			kFirstAvailableThreadID=RESERVED_THREADS;
	} while (!mark_TID_used(tid));
	return tid;
}

thread_t* createThread(void* ownerTask, bool kernelThread)
{
	thread_t* newThread;

	//Kmalloc zeroes out all memory so the thread context and other elements will be initialized to zeroes
	newThread = kmalloc(sizeof(thread_t));

	newThread->ownerTask = (void*)ownerTask;

	newThread->threadID = get_thread_id();

	//TODO: Fixme - is one of the wrong?
	newThread->regs.userCR3 = ((task_t*)ownerTask)->pml4;
	newThread->regs.CR3 = (uint64_t)((task_t*)ownerTask)->pml4;
    printd(DEBUG_THREAD,"createThread: Set thread PML4 to %p\n",newThread->regs.userCR3);

	// Note: No need to call paging_map_kernel_into_pml4() anymore!
	// Upper-half page tables are now shared directly via PML4 entries

	if (kernelThread)
	{
		newThread->regs.DS = newThread->regs.ES = newThread->regs.FS = newThread->regs.GS = newThread->regs.SS = GDT_KERNEL_DATA_ENTRY << 3;
		newThread->regs.CS = GDT_KERNEL_CODE_ENTRY << 3;
	}
	else
	{
		newThread->regs.DS = newThread->regs.ES = newThread->regs.FS = newThread->regs.GS = newThread->regs.SS = GDT_USER_DATA_ENTRY << 3 | 3;
		newThread->regs.CS = GDT_USER_CODE_ENTRY << 3 | 3;
		// Allocate user stack (ring 3) in task's lower-half address space
		newThread->esp3BaseV = (uintptr_t)task_alloc_guarded_stack((task_t*)ownerTask, THREAD_USER_STACK_SIZE, true);
		newThread->esp3BaseP = 0; // Not needed - stack is in task-specific virtual space
	    printd(DEBUG_THREAD | DEBUG_DETAILED,"Created guarded ring3 stack for thread at V=0x%016lx\n", newThread->esp3BaseV);
	}
	// Allocate kernel stack (ring 0) in task's lower-half address space
	newThread->esp0BaseV = (uintptr_t)task_alloc_guarded_stack((task_t*)ownerTask, THREAD_KERNEL_STACK_SIZE, false);
	newThread->esp0BaseP = 0; // Not needed - stack is in task-specific virtual space
	printd(DEBUG_THREAD | DEBUG_DETAILED,"Created guarded ring0 stack for thread at V=0x%016lx\n", newThread->esp0BaseV);

	printd(DEBUG_THREAD | DEBUG_DETAILED,"createThread: Initialized %s thread segment registers, CS=0x%08x, others=0x%08x\n", kernelThread?"kernel":"user", newThread->regs.CS, newThread->regs.DS);

	if (kernelThread)
	{
		//TODO: FIX ME - both RSP and RSP0 assigned kernel stack
		newThread->regs.SS = GDT_KERNEL_DATA_ENTRY << 3;
		//NOTE: The magic 6's are to leave room before the end of the stack just in case
		newThread->regs.RSP = newThread->esp0BaseV + THREAD_KERNEL_STACK_SIZE - sizeof(uintptr_t) * 6;
		newThread->regs.SS0 = GDT_KERNEL_DATA_ENTRY << 3;
		newThread->regs.RSP0 = newThread->esp0BaseV + THREAD_KERNEL_STACK_SIZE - sizeof(uintptr_t) * 6;

		// Seed the ring0 stack with task_exit_with_retval as the initial return
		// address, so that if the top-level thread function ever returns it lands
		// in the task-teardown path instead of running off the end of the stack.
		//
		// The stack lives at a task-local, lower-half VA that is NOT mapped in
		// kKernelPML4, so for tasks with their own PML4 we cannot write through
		// newThread->regs.RSP directly. Instead we translate the stack VA to its
		// physical page (walking the task's OWN page tables) and write through the
		// HHDM alias: since yesterday's lazy-HHDM change, every allocator-owned
		// page — which the guarded stack is — is reachable at phys | kHHDMOffset
		// while allocated. This works uniformly for ktask/idle (kKernelPML4) and
		// for own-PML4 tasks, and replaces the old scratch-VA temp mapping that
		// aliased the first kernel text page (0xffffffff80000000).
		task_t *task = (task_t*)ownerTask;
		uintptr_t phys_rsp = paging_walk_paging_table((pt_entry_t*)task->pml4v, newThread->regs.RSP);
		if (phys_rsp && phys_rsp != 0xbadbadba) {
			*(uintptr_t *)(phys_rsp | kHHDMOffset) = (uintptr_t)&task_exit_with_retval;
		}
	}
	else
	{
		// ─── RING3 (user) THREADS ────────────────────────────────────────────────
		// SS keeps the RPL-3 user data selector already set above — do NOT reassign
		// it here without `| 3` (that exact bug shipped once: iretq to CPL 3 with an
		// RPL-0 SS is a #GP).
		//
		// RSP: top of the USER stack, minus the traditional 6-qword safety margin
		// (matching the ring0 path), minus ONE more qword for the exit-trampoline
		// return address that task_setup_ring3_exit_path() seeds at [RSP].  That
		// seeded slot also gives _start the stack shape of a normal `call`
		// (rsp%16==8 at entry), which compiled C fixtures are allowed to assume.
		//
		// RSP0: top of the KERNEL stack — this is what the CPU (via TSS) and
		// syscall_Enter (via CLS kernel_rsp0) load on every ring3→ring0 crossing,
		// so it must be the ring0 stack, not the user one.  The -48 keeps it
		// 16-byte aligned (stack tops are page-aligned), which syscall_Enter's
		// frame math relies on.
		//
		// The exit path itself (where a ring3 `ret` from _start lands) cannot be
		// seeded here: the trampoline page isn't mapped until task_create() has
		// built the task's address space.  See task_setup_ring3_exit_path().
		newThread->regs.RSP = newThread->esp3BaseV + THREAD_USER_STACK_SIZE - sizeof(uintptr_t) * 6 - sizeof(uintptr_t);
		newThread->regs.SS0 = GDT_KERNEL_DATA_ENTRY << 3;
		newThread->regs.RSP0 = newThread->esp0BaseV + THREAD_KERNEL_STACK_SIZE - sizeof(uintptr_t) * 6;
	}

	newThread->regs.RFLAGS = 0x202;  //Interrupts enabled, reserved bit 1 set

	newThread->exited = false;
	newThread->exiting = false;
	newThread->next=NO_THREAD;
	return newThread;
}
