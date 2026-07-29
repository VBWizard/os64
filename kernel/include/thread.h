#ifndef THREAD_H
#define THREAD_H

#include <stdint.h>
#include <stdbool.h>
#include "signals.h"

#define THREAD_STACK_GUARD_PAGE_COUNT	4			//Number of pages of unmapped memory assigned to each side of a stack as a guard

#define MAX_THREADS (1024 * 1024 - 1)
#define RESERVED_THREADS 32

#define THREAD_USER_STACK_VIRTUAL_START 0x1000
#define THREAD_USER_STACK_SIZE  0x100000	//1MB user stack
#define THREAD_USER_STACK_INITIAL_VIRT_ADDRESS THREAD_USER_STACK_VIRTUAL_START + THREAD_USER_STACK_SIZE - 8

#define THREAD_KERNEL_STACK_VIRTUAL_START 0x20000
// MUST be a page multiple: the stack TOP (base + size) seeds RSP0/regs.RSP,
// and an odd size makes every kernel RSP in the OS odd.  That interacts
// fatally with the CPU's 16-byte RSP alignment on interrupt delivery — up to
// 15 bytes of live stack silently vanish per preemption.  (This was 0xFFFF,
// "64k" minus one byte; found during ring-3 bring-up as byte-SHIFTED values
// popping off resumed stacks.)
#define THREAD_KERNEL_STACK_SIZE  0x10000	//64k kernel stack
#define THREAD_KERNEL_STACK_INITIAL_VIRT_ADDRESS THREAD_KERNEL_STACK_VIRTUAL_START + THREAD_KERNEL_STACK_SIZE - 8

#define THREAD_VIRTUAL_STRUCT_ADDRESS 0xF0000000
#define NO_THREAD (void*)0xFFFFFFFFFFFFFFFF
#define THREAD_NO_AFFINITY UINT64_C(0xFFFFFFFFFFFFFFFF)

typedef enum
{
    THREAD_STATE_NONE = 0,
    THREAD_STATE_RUNNING = 1,
    THREAD_STATE_RUNNABLE = 2,
    THREAD_STATE_STOPPED = 3,
    THREAD_STATE_USLEEP = 4,
    THREAD_STATE_ISLEEP = 5,
    THREAD_STATE_ZOMBIE = 0xFF
} eThreadState;

typedef struct
{
	uint64_t R15, R14, R13, R12, R11, R10, R9, R8;
	uint64_t RDI, RSI, RBP, RDX, RCX, RBX, RAX;
	uint64_t RIP, CS, DS, ES, FS, GS, RFLAGS, RSP, SS, CR3;
	uintptr_t* userCR3;
	uint64_t SS0, RSP0;
	void *prev, *next;
} thread_context_t;

typedef struct s_thread
{
	uint64_t threadID;
	uint64_t mp_apic;
	volatile bool exited;
	bool idleThread, execDontSaveRegisters;
	volatile uint64_t retVal;
	thread_context_t regs;
	uintptr_t* pml4;
	eThreadState threadState;
	uint64_t totalRunTicks, ticksSinceLastInterrupted, prioritizedTicksInRunnable;
	uint64_t lastRunStartTicks, lastRunEndTicks, totalRunningTicks;
	uintptr_t esp0BaseV, esp0BaseP, esp0Size, esp3BaseV, esp3BaseP, esp3Size;
	void* ownerTask;
	struct s_thread *forkedThread;
	struct s_thread *prev, *next;
	signals_t signals;
	// Per-thread syscall I/O bounce block, lazily kmalloc'd by syscall.c on the
	// thread's first read()/write() and REUSED for every one after (it used to
	// be kmalloc'd/kfree'd per call — with a no-freelist allocator that was a
	// page allocation, a 4KB zero, and a TLB-shootdown IPI per call; top
	// measured it in whole seconds). Per-THREAD, not per-core: console/pipe
	// reads BLOCK while holding the buffer, and a sleeping thread's scratch
	// must not be handed to whoever runs next on the core. Opaque here — the
	// layout ([params][data]) is syscall.c's business (syscall_io_scratch()).
	// Never freed: thread teardown doesn't exist yet (the task_destroy debt);
	// this block rides along. FUTURE FORK WARNING: if fork ever copies
	// thread_t wholesale, NULL this in the child or two threads will share
	// one bounce buffer.
	void *syscallIOScratch;
} thread_t;

thread_t* createThread(void* parentTask, bool kernelThread);
uintptr_t thread_allocate_guarded_stack_memory(uintptr_t pml4, uintptr_t *virtualStart, uint64_t requestedLength, bool isRing3Stack);

#endif
