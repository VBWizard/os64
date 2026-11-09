#ifndef SMP_H
#define SMP_H

#include <stdint.h>
#include "mpspec_def.h"
#include "thread.h"
#include "tss.h"
#include "task.h"
#include "cpu_roster.h"

// A bound on CPU NUMBERS (0..kCPUCount-1, cpu_roster.h), and so the size of
// every per-CPU table. A machine with more brings up the first MAX_CPUS of the
// roster's order and says how many it left parked. 64 is a Threadripper
// 3970X; each CPU costs two GDT slots for its TSS (GDT_ENTRIES follows).
#define MAX_CPUS 64
// The roster's inverse table spells "no CPU" as 0xFF, so that byte can never
// be a CPU number.
_Static_assert(MAX_CPUS < CPU_ROSTER_NO_CPU, "MAX_CPUS must stay below kApicToCPU's sentinel");
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

// One CPU on the roster, indexed by CPU number (kCPUs). Its register
// addresses are not here: every CPU sees its OWN local APIC at the one
// address kMPApicBase holds, so a per-CPU copy could only ever be the same
// number or a wrong one.
typedef struct
{
	apic_id_t apic;          // where an IPI or an interrupt aimed at it goes
	// Limine's doorbell for THIS CPU: an address written here and the CPU
	// jumps out of the bootloader's park to it. Carried by the roster entry
	// so the wake loop can only ring the CPU it named.
	volatile uint64_t *goto_address;
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
	uint64_t cpu;									// 8: this CPU's number (cpu_roster.h)
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

	// acct = CPU-time accounting. The owning core commits each span with
	// same-core TSC subtraction. Readers use acctSequence to copy a complete
	// tuple; a remote reader never subtracts its own TSC from these stamps.
	uint64_t acctSequence;
	uint64_t acctZeroTSC;          // this core's meter epoch (first pass)
	uint64_t acctLastDispatchTSC;  // start of the uncommitted open span
	uint64_t acctSchedCycles;      // cycles spent inside scheduler passes
	uint64_t acctUserCycles;       // ring-3 time committed on this core
	uint64_t acctSystemCycles;     // thread-attributed ring-0 time on this core
	bool acctSchedSpanActive;      // open span belongs to acctSchedCycles
	bool acctSchedulerIRQOpen;      // scheduler entry stamped the outgoing thread

	// acctCurrentHalted: the CURRENT thread is parked in an sti;hlt of its
	// own making (the compositor's idle idiom — the one thread that idles
	// OUTSIDE the scheduler's view). While raised, charges redirect this
	// core's open span to acctIdleThread instead of the current thread.
	// Raised by mpAcctHaltBegin (under cli, after settling the real work),
	// dropped at interrupt entry after the nap is charged, or by
	// mpAcctHaltEnd or dispatch if no interrupt entry did so. A switched-in
	// thread is never halted. Born 2026-08-17, the day top showed guicomp at 95%
	// of a core while its flush counter proved it doing nothing — hlt-in-
	// task bills as work unless the books are told otherwise.
	bool acctCurrentHalted;
	// This core's idle THREAD — the halted spans' rightful owner. Set once
	// at idle-task creation (kernel_init); both fields are only ever read
	// and written by charge code running ON this core, so the same-core-TSC
	// discipline the meters live by is preserved automatically.
	thread_t *acctIdleThread;

	// This CPU's hardware address, for code that has to name it TO the
	// hardware. Everything else says `cpu`.
	apic_id_t apic;
} __attribute__((aligned(64))) core_local_storage_t;

// The roster (cpu_roster.h), built once by cpu_roster_init before the log
// rings and read-only from then on, so it needs no lock.
extern cpu_t kCPUs[MAX_CPUS];
extern uint32_t kCPUCount;
// APIC ID -> CPU number, CPU_ROSTER_NO_CPU for an ID that is not a numbered
// CPU. The one array an apic_id_t may index.
extern uint8_t kApicToCPU[CPU_ROSTER_APIC_IDS];
extern cpu_roster_report_t kCPURosterReport;
extern volatile uintptr_t kMPApicBase;
extern int kLocalAPICTimerSpeed[MAX_CPUS];
extern volatile core_local_storage_t kCoreLocalStorage[MAX_CPUS];
extern uintptr_t kMPICRLow;
extern uintptr_t kMPICRHigh;
extern uint64_t kMPIdReg;
extern uintptr_t kIOAPICAddress;
extern mpConfig_t* kMPConfigTable;
extern uint32_t kMPConfigTableCount;

void cpu_roster_init(void);
// The SMP boot line's tail: CPU count, the APIC range when it is not dense,
// and anything the roster left out.
void cpu_roster_print(void);
int init_SMP(bool enableSMP);

#endif
