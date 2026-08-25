#ifndef SMP_H
#define SMP_H

#include <stdint.h>
#include "mpspec_def.h"
#include "thread.h"
#include "tss.h"
#include "task.h"

#define MAX_CPUS 24
#define APIC_EOI_OFFSET    0xB0
#define AP_STACK_SIZE 0x1000
#define KERNEL_INTERRUPT_STACK_SIZE (16 * 4096)  // 64KB per-CPU kernel stack

typedef enum mpRecType
{
    CPU=0,
    BUS,
    IOAPIC,
    IOINTASS,
    LOCALINTASS
} eMPRecType;

typedef struct
{
    int apicID;
    //Virtual register base address
	uint64_t registerBase;
	//IO APIC address
	uint64_t ioAPICAddress;
	uint64_t apic_lvt_timer, apic_lvt_lint0, apic_lvt_lint1, apic_lvt_error, apic_tpr;
	uint64_t apic_id_reg, apic_svr, apic_eoi, apic_icr_low, apic_icr_high;
    uint64_t ticksPerSecond;
	//Put an address in this field and the CPU will jump out of park, to it
	void *goto_address;
} cpu_t;

typedef struct mpConfig
{
    union 
    {unsigned char rec[20];
    struct mpc_cpu cpu;
    struct mpc_bus bus;
    struct mpc_ioapic apic;
    struct mpc_intsrc irqSrc;
    struct mpc_lintsrc lintSrc;
    };
    eMPRecType recType;
    uintptr_t prevRecAddress;
    uintptr_t nextRecAddress;
    
} __attribute__((packed))mpConfig_t;

typedef struct
{
	void *self;										// 0
	uint64_t apic_id;								// 8
	uint64_t threadID;								// 16
	uint64_t apicTicksPerSecond;					// 24
	uint64_t apicTimerCount;
	uintptr_t stackVirtualAddress, stackPhysicalAddress;
	thread_t *currentThread;
	bool coreAwoken, coreInitialized;
	tss_t *tss;
	uint64_t kernel_rsp0;
    task_t *task;
	uintptr_t kernel_interrupt_stack_base;  // Upper-half kernel stack for CR3 switching
	uintptr_t kernel_interrupt_stack_top;   // Top of kernel interrupt stack

	// (Here lay cikc_saved_func/arg/cr3/rsp — the old C call_in_kernel_context's
	// per-CORE scratch, deleted 2026-08-10 with zero references left in the tree.
	// They are worth a headstone rather than a silent removal, because the reason
	// they had to die is a rule: per-CORE scratch cannot hold per-THREAD state
	// across anything a thread can be preempted inside. A thread that stashed its
	// CR3/RSP here and then resumed on a DIFFERENT core would restore whatever the
	// last user of that core's slot had left. The asm rewrite (task_exit_asm.S)
	// fixed this by keeping CR3 and RSP in callee-saved r14/r15, which the
	// scheduler saves and restores PER THREAD, so they migrate with their owner.
	// If you ever need scratch across a context switch again: registers the
	// scheduler preserves, or the thread struct — never CLS.)

	// syscall_Enter's stash for the user RSP between "SYSCALL landed" and "we're
	// on the kernel stack" (syscall.S).  GS-relative so the entry stub never has
	// to touch the user stack (a bogus ring-3 RSP must not be able to fault the
	// kernel).  Safe as a single per-core slot because SFMASK clears IF on entry:
	// nothing can preempt between the store and the reload.
	uint64_t syscall_user_rsp;

	// (The syscall RETURN FRAME pointer lived here for one day, 2026-08-23 to
	// -24, and review moved it to thread_t — where its comment now lives. The
	// short version: the frame is on the thread's KERNEL STACK, a blocking
	// syscall parks with it live, and a per-core slot kept pointing at a
	// parked thread's frame while other threads delivered signals through it.
	// syscall_user_rsp above genuinely is per-core — stored and consumed
	// nanoseconds apart under SFMASK's IF=0, before anything can park.)

	// acct = CPU-time accounting (scheduler_do's switch-boundary charging).
	// All three are written ONLY by this core, inside its own scheduler
	// pass; /sys/cpu/<n>/time reads them cross-core, which is safe for the values
	// (worst case one slice stale) as long as no reader ever SUBTRACTS a
	// remote TSC reading from a local one.
	uint64_t acctZeroTSC;          // this core's meter epoch (first pass)
	uint64_t acctLastDispatchTSC;  // when the current thread got the core
	uint64_t acctSchedCycles;      // cycles spent inside scheduler passes

	// acctCurrentHalted: the CURRENT thread is parked in an sti;hlt of its
	// own making (the compositor's idle idiom — the one thread that idles
	// OUTSIDE the scheduler's view). While raised, every charge site
	// redirects this core's span to acctIdleThread instead of the current
	// thread: halted time is idle time, whoever's rbp it happens under.
	// Raised by mpAcctHaltBegin (under cli, after settling the real work),
	// dropped by mpAcctHaltEnd AND by every dispatch (a switched-in thread
	// is never halted). Born 2026-08-17, the day top showed guicomp at 95%
	// of a core while its flush counter proved it doing nothing — hlt-in-
	// task bills as work unless the books are told otherwise.
	bool acctCurrentHalted;
	// This core's idle THREAD — the halted spans' rightful owner. Set once
	// at idle-task creation (kernel_init); both fields are only ever read
	// and written by charge code running ON this core, so the same-core-TSC
	// discipline the meters live by is preserved automatically.
	thread_t *acctIdleThread;
} __attribute__((aligned(64))) core_local_storage_t;

extern cpu_t *kCPUInfo;
extern volatile uintptr_t kMPApicBase;
extern int kLocalAPICTimerSpeed[MAX_CPUS];
extern volatile core_local_storage_t kCoreLocalStorage[MAX_CPUS];
extern volatile uint8_t kMPCoreCount;
extern uintptr_t kMPICRLow;
extern uintptr_t kMPICRHigh;
extern uint64_t kMPIdReg;
extern uintptr_t kIOAPICAddress;
extern mpConfig_t* kMPConfigTable;
extern uint32_t kMPConfigTableCount;

int init_SMP(bool enableSMP);

#endif
