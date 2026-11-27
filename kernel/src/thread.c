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

// The frames for the ring-3 stack a thread is given (SCRIPT_STACK.md § 4):
// what its program asked for, when that is from the default up to
// THREAD_USER_STACK_MAX, rounded up to a page; otherwise the default. A
// declared stack the frame table cannot give degrades to the default rather
// than failing, so it is asked of the allocator that may refuse; the
// default is asked as the caller says (`mayRefuse`). `*size` is what it got.
// The lines name the thread: its task has no ID yet, and a main thread's ID
// becomes the task's.
static uintptr_t user_stack_frames(thread_t *thread, uint64_t asked, bool mayRefuse, uint64_t *size)
{
	unsigned long tid = (unsigned long)thread->threadID;
	uint64_t declared = 0;
	if (asked > THREAD_USER_STACK_SIZE && asked <= THREAD_USER_STACK_MAX)
		declared = (asked + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
	else if (asked != 0 && asked != THREAD_USER_STACK_SIZE)
		printd(DEBUG_TASK, "createThread: thread %lu asked for a %lu-byte stack, outside %u..%u; it gets %u\n",
		       tid, asked, THREAD_USER_STACK_SIZE, THREAD_USER_STACK_MAX, THREAD_USER_STACK_SIZE);
	if (declared != 0) {
		uintptr_t phys = task_stack_frames(declared, true);
		if (phys != 0) {
			*size = declared;
			return phys;
		}
		printd(DEBUG_TASK, "createThread: thread %lu's %lu-byte stack could not be had; it gets %u\n",
		       tid, declared, THREAD_USER_STACK_SIZE);
	}
	*size = THREAD_USER_STACK_SIZE;
	return task_stack_frames(THREAD_USER_STACK_SIZE, mayRefuse);
}

// Both stacks, in task.c § Stacks' three steps: every frame, then every
// range, then the mappings, which refuse nothing. A refusal frees the frames it
// took, none of which was ever mapped, so no CPU can hold a translation of
// them; a range taken is address space, and is never handed out again.
static bool make_stacks(thread_t *t, task_t *task, bool kernelThread, uint64_t asked, bool mayRefuse)
{
	uint64_t userSize = 0;
	uintptr_t userPhys = 0, userBase = 0, kernelBase = 0;
	if (!kernelThread && (userPhys = user_stack_frames(t, asked, mayRefuse, &userSize)) == 0)
		return false;
	uintptr_t kernelPhys = task_stack_frames(THREAD_KERNEL_STACK_SIZE, mayRefuse);
	if (kernelPhys == 0) {
		if (userPhys != 0)
			free_memory(userPhys);
		return false;
	}
	if ((!kernelThread && !task_stack_reserve(task, userSize, &userBase)) ||
	    !task_stack_reserve(task, THREAD_KERNEL_STACK_SIZE, &kernelBase)) {
		if (!mayRefuse)
			panic("createThread: %s's heap range has no room for a thread's stacks\n", task->exename);
		printd(DEBUG_THREAD, "createThread: %s's heap range has no room for a thread's stacks; refused\n",
		       task->exename);
		if (userPhys != 0)
			free_memory(userPhys);
		free_memory(kernelPhys);
		return false;
	}
	if (!kernelThread)
		task_stack_map(task, userBase, userPhys, userSize, true);
	task_stack_map(task, kernelBase, kernelPhys, THREAD_KERNEL_STACK_SIZE, false);
	t->esp3BaseV = userBase;
	t->esp3BaseP = userPhys;
	t->esp3Size = userSize;
	t->esp0BaseV = kernelBase;
	t->esp0BaseP = kernelPhys;
	t->esp0Size = THREAD_KERNEL_STACK_SIZE;
	return true;
}

void thread_name_init(thread_t* thread, const char* name)
{
	thread->name[0] = '\0';
	if (name == NULL)
		return;
	// The syscall has already refused a name that breaks the rule; a kernel
	// caller's name is checked here, and one the rule refuses leaves the
	// thread unnamed rather than putting bytes in a status line that a
	// reader cannot split.
	if (!os64_thread_name_valid(name))
	{
		printd(DEBUG_THREAD, "thread_name_init: thread 0x%08lx left unnamed; its name breaks the rule\n",
		       thread->threadID);
		return;
	}
	size_t i = 0;
	for (; name[i] != '\0'; i++)
		thread->name[i] = name[i];
	thread->name[i] = '\0';
}

thread_t* createThread(void* ownerTask, bool kernelThread, uint64_t userStack, bool mayRefuse)
{
	thread_t* newThread;

	// The allocator zeroes memory, so the thread context starts initialized.
	// XSAVE/XRSTOR require the embedded FPU image to be 64-byte aligned.
	// Allocate the enclosing thread on a page boundary so that its asserted
	// fpuState member alignment holds for the object as well as its layout.
	newThread = kmalloc_aligned(sizeof(thread_t));
	if (newThread == NULL) {
		if (!mayRefuse)
			panic("createThread: no memory for %s's thread\n", ((task_t*)ownerTask)->exename);
		return NULL;
	}

	newThread->ownerTask = (void*)ownerTask;

	newThread->threadID = get_thread_id();
	fpu_state_init(&newThread->fpuState);

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
	}
	// Its stacks (task.c § Stacks): a ring-3 one for a user thread, and a
	// ring-0 one for every thread.
	if (!make_stacks(newThread, (task_t*)ownerTask, kernelThread, userStack, mayRefuse)) {
		mark_TID_unused((uint32_t)newThread->threadID);
		kfree(newThread);
		return NULL;
	}
	printd(DEBUG_THREAD | DEBUG_DETAILED,"Created guarded stacks for thread: ring3 V=0x%016lx, ring0 V=0x%016lx\n",
	       newThread->esp3BaseV, newThread->esp0BaseV);

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
		// HHDM alias: every allocator-owned page — which the stack is — is
		// reachable at phys | kHHDMOffset while allocated. This works uniformly
		// for ktask (kKernelPML4) and for own-PML4 tasks.
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
		newThread->regs.RSP = newThread->esp3BaseV + newThread->esp3Size - sizeof(uintptr_t) * 6 - sizeof(uintptr_t);
		newThread->regs.SS0 = GDT_KERNEL_DATA_ENTRY << 3;
		newThread->regs.RSP0 = newThread->esp0BaseV + THREAD_KERNEL_STACK_SIZE - sizeof(uintptr_t) * 6;
	}

	newThread->regs.RFLAGS = 0x202;  //Interrupts enabled, reserved bit 1 set

	newThread->exited = false;
	newThread->exiting = false;
	newThread->next=NO_THREAD;
	return newThread;
}
