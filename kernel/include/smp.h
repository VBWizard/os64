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

	// cikc = call_in_kernel_context (vma.c context switching scratch space)
	void (*cikc_saved_func)(void*);
	void *cikc_saved_arg;
	uint64_t cikc_saved_cr3;
	uint64_t cikc_saved_rsp;

	// syscall_Enter's stash for the user RSP between "SYSCALL landed" and "we're
	// on the kernel stack" (syscall.S).  GS-relative so the entry stub never has
	// to touch the user stack (a bogus ring-3 RSP must not be able to fault the
	// kernel).  Safe as a single per-core slot because SFMASK clears IF on entry:
	// nothing can preempt between the store and the reload.
	uint64_t syscall_user_rsp;

	// acct = CPU-time accounting (scheduler_do's switch-boundary charging).
	// All three are written ONLY by this core, inside its own scheduler
	// pass; /proc/cores reads them cross-core, which is safe for the values
	// (worst case one slice stale) as long as no reader ever SUBTRACTS a
	// remote TSC reading from a local one.
	uint64_t acctZeroTSC;          // this core's meter epoch (first pass)
	uint64_t acctLastDispatchTSC;  // when the current thread got the core
	uint64_t acctSchedCycles;      // cycles spent inside scheduler passes
} core_local_storage_t;


extern cpu_t *kCPUInfo;
extern volatile uintptr_t kMPApicBase;
extern int kLocalAPICTimerSpeed[MAX_CPUS];
extern volatile core_local_storage_t* kCoreLocalStorage;
extern volatile uint8_t kMPCoreCount;
extern uintptr_t kMPICRLow;
extern uintptr_t kMPICRHigh;
extern uint64_t kMPIdReg;
extern uintptr_t kIOAPICAddress;
extern mpConfig_t* kMPConfigTable;
extern uint32_t kMPConfigTableCount;

int init_SMP(bool enableSMP);

#endif
