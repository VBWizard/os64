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
	// THE TWO HALVES OF DYING (split 2026-08-09; they were one flag, and the
	// seam cost a day). `exited` means RETIRED — nothing left to run, the
	// scheduler may file this thread under ZOMBIE and never wake it again.
	// `exiting` means ON ITS WAY OUT — still executing the teardown, must NOT
	// be redirected down the exit trampoline a second time, and must NOT be
	// zombied yet.
	//
	// One flag served both meanings until threads arrived (2026-08-02), and
	// before that they WERE the same event: one thread per task, so
	// thread-death and task-death happened together. The threads slice split
	// the event in two and left the flag whole, which opened a window in
	// task_exit_finish — see the long note there. Setting `exited` early is
	// the bug; it tells the scheduler "safe to zombie" while the thread still
	// has the entire task teardown ahead of it.
	volatile bool exited;
	volatile bool exiting;
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
	// prev/next belong to the SCHEDULER's queues — a thread is on exactly
	// one of them at a time, and those links are rewritten every time it
	// moves. They are NOT a list of a task's threads.
	struct s_thread *prev, *next;
	// ...which is what this is (2026-08-02, os64's first ring-3 threads).
	// task->threads heads a chain of every thread the task owns, linked
	// through taskNext, so a task can have more than one without fighting
	// the run queues for the same two pointers. NULL-terminated; the first
	// thread (the one task_create builds) is always the head.
	struct s_thread *taskNext;
	signals_t signals;
	// Where syscall_Enter's 40-byte return frame sits on THIS THREAD's kernel
	// stack, for the duration of one in-flight syscall: [0]=user RFLAGS,
	// [8]=user RIP, [16]=user RSP — the three values sysretq is rebuilt from.
	// Signal delivery (SIGNALS.md §5) runs a handler by rewriting [8] and
	// [16]; sigreturn puts them back. Stored by syscall.S at entry, zeroed by
	// it on the way out, so a checkpoint reached from anywhere OTHER than a
	// syscall cannot mistake a stale frame for a live one.
	//
	// PER-THREAD, NOT PER-CORE, and it lived one day in core_local_storage
	// before review moved it here (2026-08-24). The frame is on the thread's
	// kernel stack, and a blocking syscall PARKS with the frame live: the
	// per-core slot then held a parked thread's frame while other threads ran,
	// and a woken sleeper reaching the dispatcher exit on such a core would
	// have delivered its signal into the PARKED stranger's saved return —
	// rewriting where an innocent program resumes. The same disease as the
	// cls->task staleness this arc already fixed (scheduler_load_thread):
	// per-core storage describing a per-thread fact. gs:[currentThread] always
	// names the running thread, so syscall.S reaches this in two instructions
	// and the pointer travels with the thread wherever it parks or migrates.
	uint64_t syscall_return_frame;
	// Per-thread syscall I/O bounce block, lazily kmalloc'd by syscall.c on the
	// thread's first read()/write() and REUSED for every one after (it used to
	// be kmalloc'd/kfree'd per call — with a no-freelist allocator that was a
	// page allocation, a 4KB zero, and a TLB-shootdown IPI per call; top
	// measured it in whole seconds). Per-THREAD, not per-core: console/pipe
	// reads BLOCK while holding the buffer, and a sleeping thread's scratch
	// must not be handed to whoever runs next on the core. Opaque here — the
	// layout ([params][data]) is syscall.c's business (syscall_io_scratch()).
	// Freed by task_destroy since 2026-08-06 (the undertaker buries it with
	// the thread). FUTURE FORK WARNING: if fork ever copies
	// thread_t wholesale, NULL this in the child or two threads will share
	// one bounce buffer.
	void *syscallIOScratch;
	// The core this thread was most recently DISPATCHED on — stamped by
	// scheduler_load_thread, one store, no locking (single writer: the
	// dispatching core). This is "where did it last run", not affinity
	// (mp_apic is the pin; this is the history). Surfaced in /proc so a
	// human summing per-core books can tell whose plate the time came off
	// — the question that broke Chris's idle0 arithmetic the night the
	// accounting converged (kworker's 0.5% was on core 1 all along).
	// Since 2026-08-13 it is also load-bearing: the cache-home rule in
	// scheduler_find_thread_to_run reads it as "where this thread's caches
	// are warm" and keeps the thread there until waiting beats migrating.
	uint32_t lastRunApicID;
	// When this thread last ENTERED qRunnable (kTicksSinceStart units),
	// stamped by scheduler_change_thread_queue_locked under the queue lock.
	// The cache-home rule's other operand: a thread warm on another core is
	// skipped by foreign cores until (now - runnableSinceTick) crosses
	// SCHED_MIGRATION_COST_TICKS — wall-clock, deliberately NOT the aging
	// currency (prioritizedTicksInRunnable grows per-pass, and pass rate
	// varies with core count and backstop activity; a threshold in it would
	// mean a different real duration on every machine).
	uint64_t runnableSinceTick;
	// CPU time actually spent running, in TSC cycles — charged at context-
	// switch boundaries by scheduler_do (NOT tick-sampled: a thread that
	// runs 2ms slices between ticks is invisible to sampling but not to
	// this). Written only by the core the thread runs on, using that core's
	// own TSC for both endpoints of every delta — cross-core TSC math is
	// the desync landmine and this field never commits it. Converted to
	// microseconds at /proc read time (kCPUCyclesPerSecond); raw cycles
	// never leave the kernel. Idle threads accumulate here like everyone
	// else — that is what makes "idle %" a measurement, not an assumption.
	uint64_t runCycles;
} thread_t;

thread_t* createThread(void* parentTask, bool kernelThread);
uintptr_t thread_allocate_guarded_stack_memory(uintptr_t pml4, uintptr_t *virtualStart, uint64_t requestedLength, bool isRing3Stack);
// Return a thread ID to the pool (thread.c owns the TID bitmap). Called by
// the undertaker (task_destroy) — a buried thread's ID is reusable.
bool mark_TID_unused(uint32_t tid);

#endif
