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
#define THREAD_KERNEL_STACK_SIZE  0xFFFF	//64k kernel stack
#define THREAD_KERNEL_STACK_INITIAL_VIRT_ADDRESS THREAD_KERNEL_STACK_VIRTUAL_START + THREAD_KERNEL_STACK_SIZE - 8

#define THREAD_VIRTUAL_STRUCT_ADDRESS 0xF0000000
#define NO_THREAD (void*)0xFFFFFFFFFFFFFFFF

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
	bool exited, idleThread, execDontSaveRegisters;
	uint64_t retVal;
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
} thread_t;

thread_t* createThread(void* parentTask, bool kernelThread);
uintptr_t thread_allocate_guarded_stack_memory(uintptr_t pml4, uintptr_t *virtualStart, uint64_t requestedLength, bool isRing3Stack);

#endif
