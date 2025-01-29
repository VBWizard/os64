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

extern uintptr_t kKernelBaseAddressV;
extern uintptr_t kKernelBaseAddressP;

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

thread_t* createThread(void* ownerTask, bool kernelThread)
{
	thread_t* newThread;

	//Kmalloc zeroes out all memory so the thread context and other elements will be initialized to zeroes
	newThread = kmalloc(sizeof(thread_t));

	newThread->ownerTask = (void*)ownerTask;

	newThread->threadID = get_thread_id();

	newThread->regs.userCR3 = ((task_t*)ownerTask)->pml4;
    printd(DEBUG_THREAD,"createThread: Set thread PML4 to %p\n",newThread->regs.userCR3);

	paging_map_kernel_into_pml4(((task_t*)ownerTask)->pml4v);

	if (kernelThread)
	{
		newThread->regs.DS = newThread->regs.ES = newThread->regs.FS = newThread->regs.GS = newThread->regs.SS = GDT_KERNEL_DATA_ENTRY << 3 | 3;
		newThread->regs.CS = GDT_KERNEL_CODE_ENTRY << 3 | 3;
	}
	else
	{
		newThread->regs.DS = newThread->regs.ES = newThread->regs.FS = newThread->regs.GS = newThread->regs.SS = GDT_USER_DATA_ENTRY << 3 | 3;
		newThread->regs.CS = GDT_USER_CODE_ENTRY << 3 | 3;
		newThread->esp3BaseP = (uintptr_t)allocate_memory_aligned(THREAD_USER_STACK_SIZE);
		newThread->esp3BaseV = (uintptr_t)THREAD_USER_STACK_VIRTUAL_START;
		paging_map_pages(((task_t*)ownerTask)->pml4v, newThread->esp3BaseV, newThread->esp3BaseP, THREAD_USER_STACK_SIZE / PAGE_SIZE, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
	    printd(DEBUG_THREAD | DEBUG_DETAILED,"\n");
	}
	printd(DEBUG_THREAD | DEBUG_DETAILED,"createThread: Initialized %s thread segment registers, CS=0x%08x, others=0x%08x\n", kernelThread?"kernel":"user", newThread->regs.CS, newThread->regs.DS);

	newThread->esp0BaseP = (uintptr_t)allocate_memory_aligned(THREAD_KERNEL_STACK_SIZE);
	newThread->esp0BaseV = (uintptr_t)THREAD_KERNEL_STACK_VIRTUAL_START;

	paging_map_pages(((task_t*)ownerTask)->pml4v, newThread->esp0BaseV, newThread->esp0BaseP, THREAD_KERNEL_STACK_SIZE / PAGE_SIZE, PAGE_PRESENT | PAGE_WRITE);

    printd(DEBUG_THREAD | DEBUG_DETAILED,"createThread: Stacks allocated, ESP0=0x%016lx (p=0x%016lx), ESP3=0x%016lx (p=0x%016lx)\n",
		newThread->esp0BaseV, newThread->esp0BaseP, newThread->esp3BaseV, newThread->esp3BaseP);

	if (kernelThread)
	{
		newThread->regs.SS = GDT_KERNEL_DATA_ENTRY << 3;
		newThread->regs.RSP = THREAD_KERNEL_STACK_INITIAL_VIRT_ADDRESS;
		newThread->regs.SS0 = GDT_KERNEL_DATA_ENTRY << 3;
		newThread->regs.RSP0 = THREAD_KERNEL_STACK_INITIAL_VIRT_ADDRESS;
	}
	else
	{
		newThread->regs.SS = (GDT_USER_DATA_ENTRY << 3) | 3;
		newThread->regs.RSP = THREAD_USER_STACK_INITIAL_VIRT_ADDRESS;
	}
	newThread->regs.SS0 = GDT_KERNEL_DATA_ENTRY << 3;
	newThread->regs.RSP0 = THREAD_KERNEL_STACK_INITIAL_VIRT_ADDRESS;

	newThread->regs.RFLAGS = 0x202;  //Interrupts enabled, reserved bit 1 set

	newThread->exited = false;
	newThread->next=NO_THREAD;
	return newThread;
}