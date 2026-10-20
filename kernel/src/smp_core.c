#include <stdint.h>
#include <stddef.h>
#include <cpuid.h>   // __get_cpuid — the EFER.NXE gate reads 0x80000001
#include "smp_core.h"
#include "smp_offsets.h"
#include "x86_64.h"
#include "memory/paging.h"   // pat_init_this_core — per-core WC PAT entry
#include "watchpoint.h"      // watchpoint_sync_this_core — DRs are per-CPU too
#include "CONFIG.h"
#include "msr.h"
#include "smp.h"
#include "gdt.h"
#include <stdint.h>
#include "serial_logging.h"
#include "sprintf.h"   // snprintf — the settle tripwire's line formatter
#include "strings/strcmp.h"   // strcmp — the speculation policy's keywords
#include "strings/strcpy.h"   // strncpy — remembering the policy's name
#include "exception_report.h"   // exception_wire_lock — the tripwire narrates atomically
#include "kmalloc.h"
#include "kernel.h"
#include "time.h"
#include "apic.h"
#include "gdt.h"
#include "tss.h"
#include "thread.h"
#include "accounting.h"
#include "idt.h"
#include "panic.h"
#include "fpu.h"

extern struct IDTPointer kIDTPtr;
extern void syscall_Enter();
extern volatile bool mp_inScheduler[MAX_CPUS];
extern void ap_wakeup_stub();
extern uint64_t kKernelPML4;
extern uint64_t kKernelPML4v;
extern uint64_t kHHDMOffset;
extern uint64_t kMPLVTTimer;
extern bool kTicklessScheduler;
bool kCLSInitialized = false;
bool kSMPInitDone = false;

// ── TEMP DIAG (VBox slow-motion hunt, 2026-07-30) ───────────────────────
// Counters the scheduler's once-per-second DIAG line reports. Everything
// here prices the IPI economy: raw sends, the worst ICR delivery-status
// wait (a descheduled vCPU can leave the busy bit set for MILLISECONDS on
// a real hypervisor — invisible on TCG), and settle broadcasts with their
// per-core ack timeouts. Racy unlocked increments — diagnostic tolerance.
// REMOVE when the hunt closes.
volatile uint64_t kDiagIPISends = 0;
volatile uint64_t kDiagICRMaxSpins = 0;
volatile uint64_t kDiagSettleBroadcasts = 0;
volatile uint64_t kDiagSettleTimeouts = 0;
volatile uint64_t kDiagSettleMaxSpins = 0;
volatile uint64_t kDiagSettleLastLateCPU = 0;
// Sampled by every scheduler pass ON ITS OWN CORE (LAPIC is core-local):
// which vector is actually in service (names the interrupt that drove the
// pass — timer 0x7E vs manual 0x7A vs something unexpected), and the LVT
// timer register (is the mask bit REALLY set on this implementation?).
volatile uint32_t kDiagLastVector[MAX_CPUS] = {0};
volatile uint32_t kDiagLVT[MAX_CPUS] = {0};

// TEMP DIAG: highest vector currently in service on THIS core's LAPIC.
// The ISR block is 8 dwords at 0x100..0x170, bit N of dword R = vector
// R*32+N; the highest set bit is the interrupt being serviced right now.
uint32_t apic_in_service_vector(void)
{
    for (int reg = 7; reg >= 0; reg--)
    {
        uint32_t v = read_apic_register(kMPApicBase + 0x100 + reg * 0x10);
        if (v)
        {
            for (int b = 31; b >= 0; b--)
                if (v & (1U << b))
                    return (uint32_t)(reg * 32 + b);
        }
    }
    return 0;
}
// AP bring-up's scratch: these, tempStack and tempCls below are GLOBALS
// that every AP uses in turn — sound only because ap_wake_up_aps brings APs
// through strictly one at a time (it rings the next doorbell after the last
// AP's coreInitialized handshake). ap_wakeup_entry keeps its state here
// rather than in locals because it switches stacks under its own frame
// pointer, and a local would live on the bootloader's stack it just left.
uintptr_t stackVirtualAddress, stackPhysicalAddress;
uintptr_t kMPEOIOffset = 0;
// The AP bootstrap stack. ONE, shared, for the reason above.
//
// 32KB, and the size is a scar: this was 1KB for years, and ap_wakeup_entry
// runs real C on it — kmalloc_aligned, paging_map_pages, and (under
// DEBUG_DETAILED) a printd per page-table entry, whose varargs save area,
// sprintf frame, and message buffer stack up fast. The overflow marched out
// the bottom of the 1KB array and scribbled single qwords across the
// mp_isrSaved* register arrays that link just below it in .bss — which
// surfaced as the intermittent /idle #GP/#DB/#UD that went unexplained for
// WEEKS, because the victim was whichever core's saved state the frames
// happened to land on. tss.c had even documented this exact failure inside
// tss_initialize_cpu and been cured locally — while this file kept feeding
// the same 1KB stack to the rest of bring-up. Full forensics:
// SCHEDULER_STRAY_WRITE.md ("2026-08-03: the culprit").
//
// The canary strip below is the tripwire that would have named this bug the
// first week: 64 bytes of known pattern at the stack's floor, verified after
// every AP handshake. A sparse frame can hop over one qword; it cannot miss
// eight.
#define TEMP_STACK_SIZE (32 * 1024)
#define TEMP_STACK_CANARY 0x43414E4152592121ULL   // "CANARY!!" — legible in a hex dump
#define TEMP_STACK_CANARY_QWORDS 8
__attribute__((aligned(16))) uint8_t tempStack[TEMP_STACK_SIZE];
uint32_t temp_cpu;   // the waking AP's CPU number
core_local_storage_t *tempCls;

// Assuming kMPApicBase and APIC_EOI_OFFSET are properly defined elsewhere
extern volatile uintptr_t kMPApicBase;
extern void _write_eoi();

#define APIC_EOI_OFFSET 0xB0

// EPITAPH (2026-07-11): here lay SMP_MAGIC_NUMBER (3), which multiplied every
// LAPIC timer arming below and silently divided the scheduler to ~33
// passes/sec for most of this project's life. The investigation that killed
// it proved the calibration, the arming, the PIT, and the tick clock all
// EXACT — the multiplier was actually rationing DEBUG_SCHEDULER's printd
// volume: string formatting inside kSchedulerSwitchTasksLock inside the
// un-EOI'd scheduler interrupt convoyed every core (~195 passes/sec
// system-wide cap, single passes up to 1.3s) and starved IRQ0 down to ~48
// ticks/sec (the LAPIC pending bit holds exactly ONE tick). Full autopsy in
// SCHEDULER.md. Cure the pass COST, never the pass RATE — do not reintroduce
// a multiplier here.

void write_eoi() {
    __asm__ volatile (
        "push rax\n\t"
        "push rcx\n\t"

        // Write the End-of-Interrupt (EOI) value
        "mov rcx, %[eoi_register]\n\t" // Load EOI register address into rcx
        "mov dword ptr [rcx], 0\n\t"   // Write 0 to the EOI register

        "pop rcx\n\t"
        "pop rax\n\t"
        :
        : [eoi_register] "r"(kMPApicBase | APIC_EOI_OFFSET) // Input operand
        : "memory" // Clobber memory to enforce memory-mapped I/O order
    );
//    *((volatile uint32_t*)(kMPApicBase | APIC_EOI_OFFSET)) = 0;

}

void send_ipi(uint32_t cpu, uint32_t vector, uint32_t delivery_mode, uint32_t level, uint32_t trigger_mode)
{
	core_local_storage_t *sender_cls = get_core_local_storage();
	uint32_t sender_cpu = sender_cls ? (uint32_t)sender_cls->cpu : BSP_CPU;
	// A number off the roster has no APIC ID to aim at, and would read past
	// every per-CPU table on the way: a caller handing one in is the bug.
	if (cpu >= kCPUCount)
		panic("send_ipi: AP%u sent vector 0x%02x to CPU %u, which is not on the roster (%u CPUs)",
		      sender_cpu, vector, cpu, kCPUCount);
	if (mp_inScheduler[cpu] && vector == IPI_MANUAL_SCHEDULE_VECTOR)
	{
		printd(DEBUG_SMP | DEBUG_DETAILED,"MP: send_ipi_int - NOT sending an scheduling IPI because we're already in the scheduler");
		return;
	}
    printd(DEBUG_SMP | DEBUG_DETAILED,"MP: AP%u sending IPI 0x%02x to AP%u\n",sender_cpu, vector, cpu);
    // Ensure previous IPI command has completed — with a bound. An IPI whose
    // delivery-status bit never clears (wedged target core, virtual APIC
    // quirk) previously spun here forever, silently; a loud panic with the
    // vector and sender beats an invisible hang every time.
    uint64_t icr_wait_spins = 0;
    while (*((volatile uint32_t*)(kMPICRLow)) & 0x01000)
    {
        if (++icr_wait_spins > 100000000UL)
            panic("send_ipi: ICR delivery-status stuck busy — AP%u sending vector 0x%02x to AP%u never completed", sender_cpu, vector, cpu);
        __builtin_ia32_pause();
    }
    kDiagIPISends++;                                   // TEMP DIAG
    if (icr_wait_spins > kDiagICRMaxSpins)             // TEMP DIAG
        kDiagICRMaxSpins = icr_wait_spins;             // TEMP DIAG

    // The ICR names its target by APIC ID: this is the hardware door.
    *((volatile uint32_t*)(kMPICRHigh)) = kCPUs[cpu].apic.id << 24;

    // Write to the low part of the ICR (command and vector)
	// Removed setting of level bit 14 (| (level << 14) )
    uint32_t icr_low_value = vector | (delivery_mode << 8) | (level << 14) | (trigger_mode << 15) | 0x00004000;
    *((volatile uint32_t*)(kMPICRLow)) = icr_low_value;
    printd(DEBUG_SMP | DEBUG_DETAILED,"MP: AP%u programmed ICR for AP%u (0x%04x)\n",sender_cpu, cpu, vector);
}

// cpu_self (smp_core.h). Before ANY CPU has a CLS only the BSP runs, so
// that answer is CPU 0 and needs no register at all; an AP between waking
// and publishing its CLS asks its local APIC and the roster.
uint32_t cpu_self(void)
{
	core_local_storage_t *cls = try_get_core_local_storage();
	if (cls)
		return (uint32_t)cls->cpu;
	if (!kCLSInitialized)
		return BSP_CPU;
	return kApicToCPU[read_apic_id().id];
}

static inline void set_gs_base(uint64_t base) {
	wrmsr64(IA32_GS_BASE, base);
	kCLSInitialized = true;
}

/// @brief Which core's scratch ("kernel interrupt") stack contains `rsp`? -1 = none.
///
/// The dispatch tripwire's oracle (scheduler_load_thread). The per-core
/// scratch stack is bolted to its core — it hosts task_exit's last breath and
/// call_in_kernel_context's borrowed frames — and a context PARKED on one is
/// a time bomb: while it sleeps, the owning core re-issues the same stack to
/// its next customer, and whichever core later resumes the sleeper puts two
/// live contexts on one stack. That collision was the stack poisoner
/// (2026-08-11/12); the exit path is cured, and this oracle exists to catch
/// any door we haven't found. The cikc-hosted disk closes were the named
/// suspects for eight days and the suspicion was CORRECT: task_destroy's
/// burial close reached call_in_kernel_context from kworker with IF=1, so a
/// scheduling IPI could park kworker right here (found in review, 2026-08-20).
/// That door is shut — the trampoline is irqsave now (task_exit_asm.S) — but
/// the oracle stays: it is cheap, and it earned its keep once already.
int kernel_scratch_stack_owner(uintptr_t rsp)
{
    for (uint32_t c = 0; c < kCPUCount; c++) {
        volatile core_local_storage_t *cls = &kCoreLocalStorage[c];
        if (cls->kernel_interrupt_stack_base != 0 &&
            rsp >= cls->kernel_interrupt_stack_base &&
            rsp <  cls->kernel_interrupt_stack_top)
            return (int)c;
    }
    return -1;
}

void init_core_local_storage(uint32_t cpu)
{
	if (cpu >= kCPUCount)
		panic("init_core_local_storage: CPU %u is not on the roster (%u CPUs)", cpu, kCPUCount);

	uint64_t coreBase = (uint64_t)kCoreLocalStorage +
                        (cpu * sizeof(core_local_storage_t));
    set_gs_base(coreBase);
	core_local_storage_t *cls = (core_local_storage_t*)coreBase;
	// Who this CPU is goes in BEFORE `self`: try_get_core_local_storage
	// trusts a slot only once self points at it, and cpu_self reads the
	// number the moment it does.
	cls->cpu = cpu;
	cls->apic = kCPUs[cpu].apic;
	cls->self = cls;
	cls->tss = tss_get_for_cpu(cpu);
	cls->kernel_rsp0 = cls->tss ? cls->tss->rsp0 : 0;

	// Allocate upper-half kernel interrupt stack for CR3 switching
	void *kernel_stack = kmalloc_aligned(KERNEL_INTERRUPT_STACK_SIZE);
	if (!kernel_stack)
		panic("Failed to allocate kernel interrupt stack for CPU %u", cpu);

	cls->kernel_interrupt_stack_base = (uintptr_t)kernel_stack;
	cls->kernel_interrupt_stack_top = (uintptr_t)kernel_stack + KERNEL_INTERRUPT_STACK_SIZE;

	kMPEOIOffset = kMPApicBase | APIC_EOI_OFFSET;
	printd(DEBUG_THREAD | DEBUG_DETAILED, "Core local storage initialized to 0%16lx for CPU %u (kernel stack: 0x%lx-0x%lx)\n",
		coreBase, cpu, cls->kernel_interrupt_stack_base, cls->kernel_interrupt_stack_top);
}

// Called to finish initializing the AP (stack switch has been done in ap_wakeup_entry())
void ap_wakeup_after_stack_switch(uint32_t cpu, uint64_t stackVirtualAddress, uint64_t stackPhysicalAddress)
{
    volatile core_local_storage_t *temp_cls = get_core_local_storage();
    temp_cls->stackVirtualAddress = stackVirtualAddress;
    temp_cls->stackPhysicalAddress = stackPhysicalAddress;

    // The round trip, checked on the CPU itself: the slot it published is
    // the number the roster gave it, and that number's APIC ID is the one
    // its own local APIC reports. Either failing means some CPU is about to
    // run on another's per-CPU state.
    apic_id_t mine = read_apic_id();
    if (temp_cls->cpu != cpu || kCPUs[cpu].apic.id != mine.id || kApicToCPU[mine.id] != cpu)
        panic("AP bring-up: CPU %u's slot says %lu, the roster says APIC %u, its LAPIC says APIC %u (-> CPU %u)",
              cpu, temp_cls->cpu, kCPUs[cpu].apic.id, mine.id, kApicToCPU[mine.id]);

    printd(DEBUG_SMP, "AP%u: Initial stack v/p: 0x%016x/0x%016x\n",cpu, stackVirtualAddress, stackPhysicalAddress);

    // Read APIC_BASE_MSR
    uint32_t lvt_timer = read_apic_register(kMPApicBase + APIC_LVT_TIMER);
    uint32_t spurious_vector = read_apic_register(kMPApicBase + APIC_SPURIOUS_VECTOR);
    printd(DEBUG_SMP, "AP%u: LVT_TIMER = 0x%08x, SPURIOUS_VECTOR = 0x%08x\n", cpu, lvt_timer, spurious_vector);

	// Every register below is this CPU's OWN local APIC, which it reaches at
	// kMPApicBase like every other CPU reaches its own.
	*((volatile uint32_t*)(kMPICRHigh)) = mine.id << 24;  // Set destination APIC ID
	*((volatile uint32_t*) (kMPApicBase + 0x80)) = 0x30;  // Correct TPR
	__asm__ volatile ("mfence");  // Ensure memory writes complete

	// PAT entry 7 = write-combining on THIS core, same as the BSP set before
	// the kernel tables were built (kernel.c) — IA32_PAT is per-core and the
	// SDM wants uniformity. Must happen before this core ever stores through
	// the framebuffer's PAGE_WC mapping (the compositor runs on APs).
	pat_init_this_core();

	// And this core's debug registers, from the same global table every other
	// core is running: DR0-3/DR7 are per-CPU, so a core that came up after a
	// watchpoint was armed would otherwise be the one blind spot in the
	// machine — and the corruption always happens on the core you weren't
	// watching. (watchpoint.h, limit 3.)
	watchpoint_sync_this_core();

	*((volatile uint32_t*)(kMPApicBase + 0xF0)) |= 0x100; // Set bit 8 (Enable LAPIC)
	//EOI to clear out the IRR as we don't know what is awaiting us when we enable the APIC/LVT otherwise
	*((volatile uint32_t*)(kMPApicBase + APIC_EOI_OFFSET)) = 0;

	// Debugging: Check if AP is ready to receive IPI
    printd(DEBUG_SMP | DEBUG_DETAILED, "AP%u: Ready to receive IPI? APIC_STATUS = 0x%08x\n", cpu, *((volatile uint32_t*)kMPICRLow));

	// Set the spurious vector to 0xFF and enable interrupts (bit 8)
	*((volatile uint32_t*) (kMPApicBase + 0xF0)) = 0x1FF;  // Enable APIC + Set spurious vector to 0xFF
	__asm__ volatile ("mfence");  // Ensure memory writes complete

	// Debugging: Confirm that AP is now ready to receive IPIs
	printd(DEBUG_SMP | DEBUG_DETAILED, "AP%u: Ready to receive IPI? APIC_STATUS = 0x%08x\n", cpu, *((volatile uint32_t*)kMPICRLow));

	printd(DEBUG_SMP | DEBUG_DETAILED, "AP%u: LVT before: 0x%08x\n", cpu, *((volatile uint32_t*)kMPLVTTimer));
	
	// Unmask LVT0 (timer) and LVT1 (error) by clearing the mask bit (bit 16)
	*((volatile uint32_t*)kMPLVTTimer) &= ~0x10000;  // Unmask LVT0 (timer)
	*((volatile uint32_t*)kMPLVTTimer) &= ~0x20000;  // Unmask LVT1 (error)
	__asm__ volatile ("mfence");  // Ensure memory writes complete
	// Debugging: Confirm LVT lines are unmasked
	printd(DEBUG_SMP | DEBUG_DETAILED, "AP%u: LVT after: 0x%08x\n", cpu, *((volatile uint32_t*)kMPLVTTimer));

	// Now the AP is ready to receive and process the IPI
	temp_cls->coreAwoken = true;
}

void ap_wakeup_entry() {
    __asm__(
        "mov rsp, %2\n"    // Set RSP safely
        "mov cr3, %0\n"    // Load CR3 first (so AX isn’t clobbered)
        "mov ax, %1\n"     // Load Kernel Data Segment (0x30)
        "mov ds, ax\n"
        "mov es, ax\n"
        "mov fs, ax\n"
        "mov ss, ax\n"
        :: "r" (kKernelPML4), "r" ((uint16_t)0x30), "r" (tempStack + TEMP_STACK_SIZE - 8)
    );

    // Who am I? The local APIC says where; the roster says which CPU that
    // is. A miss means Limine started a CPU the roster refused, and it must
    // stop HERE, before the number indexes anything — the first consumer is
    // the very next call.
    temp_cpu = kApicToCPU[read_apic_id().id];
    if (temp_cpu == CPU_ROSTER_NO_CPU)
        panic("AP bring-up: a CPU woke with APIC ID %u, which is not on the CPU roster",
              read_apic_id().id);

    // Set up the rest of the AP initialization
    load_gdt_and_jump(&kGDTr);
    tss_initialize_cpu(temp_cpu);
	fpu_init_this_cpu();
    asm volatile ("lidt %0" : : "m" (kIDTPtr));

	// Set up the AP stack
    stackVirtualAddress = (uintptr_t)kmalloc_aligned(AP_STACK_SIZE);
    stackPhysicalAddress = stackVirtualAddress & ~(kHHDMOffset);

    __asm__("mov rsp, %0\n"::"r" (stackVirtualAddress + AP_STACK_SIZE - sizeof(uintptr_t)));
	
    tss_set_rsp0(temp_cpu, stackVirtualAddress + AP_STACK_SIZE - sizeof(uintptr_t));

    init_core_local_storage(temp_cpu);

	tempCls = get_core_local_storage();
    if (tempCls)
    {
        tempCls->kernel_rsp0 = stackVirtualAddress + AP_STACK_SIZE - sizeof(uintptr_t);
    }

	// Initialize the AP after stack switch (set spurious vector, enable interrupts, etc.)
    ap_wakeup_after_stack_switch(temp_cpu, stackVirtualAddress, stackPhysicalAddress);

    // Loop to ensure the AP doesn't fall off the function
    while (1) {
        __asm__("sti\nhlt\n");  // Enable interrupts and halt the AP
    }
}
void ap_wake_up_aps() {
	volatile core_local_storage_t *cls;

	// Arm the bootstrap-stack canary strip (see tempStack's comment): 64
	// bytes of pattern at the stack's FLOOR, checked after every AP's
	// handshake. Any AP whose bring-up frames reach the floor smashes the
	// pattern before it can underflow into the .bss neighbors below.
	for (int q = 0; q < TEMP_STACK_CANARY_QWORDS; q++)
		((volatile uint64_t *)tempStack)[q] = TEMP_STACK_CANARY;

	// CPU 0 is the BSP, already running; everyone else in roster order. Each
	// CPU is woken through ITS OWN doorbell and waited for in ITS OWN slot,
	// both found by the one number — the wake loop cannot ring one CPU and
	// listen for another.
	for (uint32_t cpu = 1; cpu < kCPUCount; cpu++) {
        printd(DEBUG_SMP, "MP: Waking up AP%u (APIC %u)\n", cpu, kCPUs[cpu].apic.id);
			// BOOTMARK mile-markers (kernel.h) — permanent, BOOTMARKS-gated.
			// Per-AP resolution because AP bring-up was the 54-second culprit.
			if (kEnableBootmarks)
				printf("BOOTMARK ap%u-wake              tick=%lu tsc=%lu\n", cpu, kTicksSinceStart, rdtsc());
			*kCPUs[cpu].goto_address = (uint64_t) &ap_wakeup_entry;

			cls = get_core_local_storage_for_core(cpu);

			while (!cls->coreAwoken) {wait(10);}
			if (kEnableBootmarks)
				printf("BOOTMARK ap%u-awoken            tick=%lu tsc=%lu\n", cpu, kTicksSinceStart, rdtsc());
			send_ipi(cpu, IPI_AP_INITIALIZATION_VECTOR, 0, 1, 0);
			while (!cls->coreInitialized) {wait(10);}
			if (kEnableBootmarks)
				printf("BOOTMARK ap%u-initialized       tick=%lu tsc=%lu\n", cpu, kTicksSinceStart, rdtsc());

			// The AP is up and off the bootstrap stack — verify it never
			// touched the floor. A smashed canary means bring-up came
			// within 64 bytes of repeating the /idle stray-write saga
			// (SCHEDULER_STRAY_WRITE.md), and that is a stop-the-line
			// event, not a log line: the .bss below may already be dirty.
			for (int q = 0; q < TEMP_STACK_CANARY_QWORDS; q++)
				if (((volatile uint64_t *)tempStack)[q] != TEMP_STACK_CANARY)
					panic("AP %u bring-up smashed the tempStack canary (qword %u = 0x%016lx) — grow TEMP_STACK_SIZE; see SCHEDULER_STRAY_WRITE.md\n",
						cpu, q, ((volatile uint64_t *)tempStack)[q]);
			if (kTicklessScheduler)
			{
				// Tickless (wake-on-work): park AP timers and kick each AP once so it can run its idle thread.
				send_ipi(cpu, IPI_MANUAL_SCHEDULE_VECTOR, 0, 1, 0);
			}
			else
			{
				send_ipi(cpu, IPI_ENABLE_SCHEDULING_VECTOR, 0, 1, 0);
			}
	    }
		kSMPInitDone = true;
}

void ap_enable_schedulers() {
    for (uint32_t cpu = 1; cpu < kCPUCount; cpu++) {   // CPU 0, the BSP, enables its own
        printd(DEBUG_SMP, "MP: Enabling scheduling on AP%u\n", cpu);
        if (kTicklessScheduler)
            send_ipi(cpu, IPI_MANUAL_SCHEDULE_VECTOR, 0, 1, 0);
        else
            mp_enable_scheduling_vector(cpu);
    }
}

uint32_t ap_get_timer_ticks_per_interval(int ticksToWait)
{
    //Determine the numer of times the local apic timer ticks per second
    //Use kTicksSinceStart which is updated at a frequency of TICKS_PER_SECOND (100 currently), so every 10 MS
    //Start the local APIC timer and wait for ticksToWait ticks to pass
    write_apic_register(kMPApicBase + APIC_TIMER_DIVIDE_CONFIG, 0x3);  // Set divide configuration - divisor = 16
    write_apic_register(kMPApicBase + APIC_LVT_TIMER, 0x20000);        // One-shot mode
    write_apic_register(kMPApicBase + APIC_TIMER_INIT_COUNT, 0xFFFFFFFF);  // Large count
    uint32_t end = kTicksSinceStart + ticksToWait;
    printd(DEBUG_SMP, "DEBUG: Before wait\n");
    // pause, not nop: this is a hot spin on a shared variable, and under a
    // hypervisor a nop-spin can keep this vCPU pegged while the BSP (whose
    // timer ISR advances kTicksSinceStart) is descheduled by the HOST —
    // stretching this "100ms" wait many times over (the 54-second VBox boot).
    while (kTicksSinceStart < end)
	{
		__builtin_ia32_pause();
	}
    //Read the current count
    uint32_t count=read_apic_register(kMPApicBase + APIC_TIMER_CURRENT_COUNT);
    printd(DEBUG_SMP, "DEBUG: After wait\n");
    //Calculate the speed
    uint32_t localAPICSpeed = (0xFFFFFFFF - count); 
	uint32_t MSPerTick = 1000 / TICKS_PER_SECOND;
    printd(DEBUG_SMP, "getAPICTicksPerSecond: %u ticks passed in %u MS (based on count of 0x%08x)\n", localAPICSpeed,MSPerTick * ticksToWait,count);
    return localAPICSpeed;
}

void mp_determine_local_APIC_timer_speed()
{
	core_local_storage_t *cls = get_core_local_storage();
    printd(DEBUG_SMP, "getAPICTicksPerSecond: Determining local APIC timer frequency\n");
    //Get apic timer ticks per second 3 times and average the sum
    for (int i = 0; i < TIMER_SYNC_ITERATIONS; i++) {
		//Get # of ticks for 100 MS and multiply that by 10
        cls->apicTicksPerSecond += (ap_get_timer_ticks_per_interval(TICKS_PER_SECOND / 10) * 10); //10 ticks is 100 ms or 1/10 second
    }
    cls->apicTicksPerSecond /=TIMER_SYNC_ITERATIONS;
    // Defuse the calibration timer: the one-shot rounds above leave the LVT
    // armed UNMASKED with vector 0 and a ~68-second fuse (count 0xFFFFFFFF at
    // div16) — an illegal-vector delivery waiting to happen on any boot that
    // idles long enough before the scheduler reprograms it. Mask it here;
    // ap_configure_scheduler_timer sets the real vector/mode later.
    write_apic_register(kMPApicBase + APIC_LVT_TIMER, DISABLE_TIMER(0));
    int displayedSpeed = cls->apicTicksPerSecond/1000/1000;
    printd(DEBUG_SMP, "Local APIC timer *adjusted* frequency is %u MHz (average %u ticks per second)\n", displayedSpeed,cls->apicTicksPerSecond);
}

// ── SPEC_CTRL is the kernel's to write, on every thread (2026-10-18) ─────────
//
// IA32_SPEC_CTRL (MSR 0x48) carries the speculation controls an operating
// system chooses — IBRS, STIBP, SSBD, PSFD — and it is PER HARDWARE THREAD.
// os64 used to inherit it, and the P5's firmware (Ryzen 5 6600H) hands the
// SECOND thread of every core SSBD set while the first thread's is clear —
// because it writes the register on the first thread of each core and never
// on the second, which keeps whatever it held: the reset value on a cold
// boot, the last policy across a warm one (the boot line shows both).
// Measured with /tests/coreprobe: thread 1 paid about six cycles for every
// branch instruction, taken or not, a 2.5x loss on any loop — which made the
// compositor three times slower per pixel whenever it landed on an odd APIC
// and sent yonder spinning on kGuiLock (the per-boot "bad state" of
// 2026-10-17/18). Every other instrument read "idle" and "halted"; the
// /sys/cpu/<n>/probe MSR lines found the one register the two threads of a
// core disagreed on.
//
// The policy is the one Linux and Windows apply — the kernel decides the
// value and writes the same one on every thread — and the VALUE is a system
// setting: `speculation =` in /etc/os64.conf (Chris, 2026-10-18: "default
// it to make the OS performant", and when a mitigation is ever wanted it is
// one line, applied evenly, not a firmware menu's accident).
//
//   fast      0 — no controls. The default, and what bring-up writes before
//             any file can be read. os64 runs no sandboxed untrusted code
//             that these mitigations protect, and the cost here is measured.
//   guarded   every control this CPU reports: IBRS, STIBP, SSBD, PSFD.
//   0x<hex>   the bits themselves, for the person who knows which one;
//             bits the CPU does not report are dropped, and said so.
//
// Bring-up writes `fast` standing on each thread; the root file, read after
// SMP is up, applies anything else through IPI_SPEC_CTRL_VECTOR, because the
// register can only be written from the thread it belongs to. What each
// thread inherited is kept and reported after bring-up and again when the
// root file changes the policy (spec_ctrl_report), so a firmware that does
// this again is seen on the glass instead of felt.
#define SPEC_CTRL_MSR    0x48
#define SPEC_CTRL_IBRS   (1ull << 0)
#define SPEC_CTRL_STIBP  (1ull << 1)
#define SPEC_CTRL_SSBD   (1ull << 2)
#define SPEC_CTRL_PSFD   (1ull << 7)

volatile uint64_t kSpecCtrlInherited[MAX_CPUS];
volatile uint64_t kSpecCtrlApplied[MAX_CPUS];   // what each thread last wrote
volatile uint64_t kSpecCtrlValue;               // the policy in force
volatile bool kSpecCtrlPresent;
static char kSpecCtrlPolicy[24] = "fast";

// The bits this CPU implements, by CPUID. Intel: leaf 7 EDX IBRS/IBPB[26],
// STIBP[27], SSBD[31], PSFD in subleaf 2's EDX[0]. AMD: leaf 0x80000008
// EBX IBRS[14], STIBP[15], SSBD[24], PSFD[28]. Any bit set means the MSR
// itself is implemented.
static uint64_t spec_ctrl_supported(void)
{
	uint32_t a = 0, b = 0, c = 0, d = 0;
	uint64_t mask = 0;
	if (__get_cpuid_count(7, 0, &a, &b, &c, &d)) {
		uint32_t maxsub = a;
		if (d & (1u << 26)) mask |= SPEC_CTRL_IBRS;
		if (d & (1u << 27)) mask |= SPEC_CTRL_STIBP;
		if (d & (1u << 31)) mask |= SPEC_CTRL_SSBD;
		// Intel enumerates PSFD one subleaf over: CPUID.(7,2):EDX[0].
		if (maxsub >= 2 && __get_cpuid_count(7, 2, &a, &b, &c, &d) && (d & 1u))
			mask |= SPEC_CTRL_PSFD;
	}
	if (__get_cpuid(0x80000008, &a, &b, &c, &d)) {
		if (b & (1u << 14)) mask |= SPEC_CTRL_IBRS;
		if (b & (1u << 15)) mask |= SPEC_CTRL_STIBP;
		if (b & (1u << 24)) mask |= SPEC_CTRL_SSBD;
		if (b & (1u << 28)) mask |= SPEC_CTRL_PSFD;
	}
	return mask;
}

static void spec_ctrl_write_this_cpu(uint32_t cpu)
{
	uint64_t value = kSpecCtrlValue;
	if (kSpecCtrlPresent)
		wrmsr64(SPEC_CTRL_MSR, value);
	if (cpu < MAX_CPUS)
		kSpecCtrlApplied[cpu] = value;
}

static void spec_ctrl_init_this_cpu(uint32_t cpu)
{
	if (spec_ctrl_supported() == 0)
		return;
	kSpecCtrlPresent = true;
	uint64_t inherited = rdmsr64(SPEC_CTRL_MSR);
	if (cpu < MAX_CPUS)
		kSpecCtrlInherited[cpu] = inherited;
	spec_ctrl_write_this_cpu(cpu);
	printd(DEBUG_SMP, "AP%u: SPEC_CTRL inherited 0x%lx, set to 0x%lx\n", cpu, inherited, kSpecCtrlValue);
}

void spec_ctrl_apply_ISR(void)
{
	core_local_storage_t *cls = get_core_local_storage();
	spec_ctrl_write_this_cpu(cls ? (uint32_t)cls->cpu : MAX_CPUS);
	write_eoi();
}

static bool spec_ctrl_parse_hex(const char *value, uint64_t *out)
{
	if (value[0] != '0' || (value[1] != 'x' && value[1] != 'X') || value[2] == 0)
		return false;
	uint64_t v = 0;
	for (const char *c = value + 2; *c; c++) {
		uint64_t digit;
		if (*c >= '0' && *c <= '9') digit = (uint64_t)(*c - '0');
		else if (*c >= 'a' && *c <= 'f') digit = (uint64_t)(*c - 'a' + 10);
		else if (*c >= 'A' && *c <= 'F') digit = (uint64_t)(*c - 'A' + 10);
		else return false;
		if (v >> 60)
			return false;
		v = (v << 4) | digit;
	}
	*out = v;
	return true;
}

bool spec_ctrl_set_policy(const char *value)
{
	if (value == NULL)
		return false;
	if (!kSpecCtrlPresent) {
		printd(DEBUG_BOOT, "SPEC_CTRL: speculation = %s ignored: not implemented on this CPU\n", value);
		return false;
	}
	uint64_t supported = spec_ctrl_supported();
	uint64_t want;
	if (strcmp(value, "fast") == 0) {
		want = 0;
	} else if (strcmp(value, "guarded") == 0) {
		want = supported;
	} else if (spec_ctrl_parse_hex(value, &want)) {
		if (want & ~supported) {
			printd(DEBUG_BOOT, "SPEC_CTRL: speculation = %s: bits 0x%lx are not implemented here, dropped\n",
			       value, want & ~supported);
			want &= supported;
		}
	} else {
		printd(DEBUG_BOOT, "SPEC_CTRL: speculation = %s is not fast, guarded or 0x<hex>: keeping %s\n",
		       value, kSpecCtrlPolicy);
		return false;
	}
	strncpy(kSpecCtrlPolicy, value, sizeof(kSpecCtrlPolicy));
	kSpecCtrlPolicy[sizeof(kSpecCtrlPolicy) - 1] = 0;
	kSpecCtrlValue = want;

	// Identify the thread and write it with interrupts off: this task is
	// not pinned, and a preemption between learning which thread it is on
	// and writing would record one thread as done while another was written.
	uint64_t irq;
	__asm__ volatile("pushfq\n\tpop %0\n\tcli" : "=r"(irq) :: "memory");
	core_local_storage_t *cls = get_core_local_storage();
	uint32_t self = cls ? (uint32_t)cls->cpu : BSP_CPU;
	spec_ctrl_write_this_cpu(self);
	if (irq & 0x200)
		__asm__ volatile("sti" ::: "memory");
	if (!kSMPInitDone)
		return true;   // nothing else is up; bring-up will write this value
	for (uint32_t cpu = 0; cpu < kCPUCount; cpu++) {
		if (cpu != self)
			send_ipi(cpu, IPI_SPEC_CTRL_VECTOR, 0, 1, 0);
	}
	// Bounded, and only with interrupts on (mpAcctSettleAll's rule: never
	// wait for an interrupt-delivered answer with them off). A thread that
	// has not confirmed by the report simply shows there as unconfirmed.
	if (irq & 0x200) {
		uint64_t deadline = kTicksSinceStart + 10;
		for (uint32_t cpu = 0; cpu < kCPUCount; cpu++) {
			while (kSpecCtrlApplied[cpu] != want &&
			       kTicksSinceStart < deadline)
				__builtin_ia32_pause();
		}
	}
	// The root file is read after bring-up's report has printed, so say it
	// again: the glass must show the policy that is actually in force.
	spec_ctrl_report();
	return true;
}

// The boot line, glass and log: the policy in force, how many threads
// confirmed it, and how many arrived with controls already set. Inherited
// non-zero is not an error — it is the firmware's choice being overruled,
// and the number worth knowing the day a machine is slow on half its CPUs.
void spec_ctrl_report(void)
{
	if (!kSpecCtrlPresent) {
		printf("SPEC_CTRL: not implemented on this CPU\n");
		printd(DEBUG_BOOT, "SPEC_CTRL: not implemented on this CPU\n");
		return;
	}
	uint32_t nonzero = 0, first = 0, confirmed = 0;
	uint64_t firstVal = 0;
	for (uint32_t cpu = 0; cpu < kCPUCount; cpu++) {
		if (kSpecCtrlApplied[cpu] == kSpecCtrlValue)
			confirmed++;
		if (kSpecCtrlInherited[cpu] != 0) {
			if (nonzero == 0) {
				first = cpu;
				firstVal = kSpecCtrlInherited[cpu];
			}
			nonzero++;
		}
	}
	char line[160];
	if (nonzero == 0)
		sprintf(line, "SPEC_CTRL: %s (0x%lx) on %u of %u threads; none inherited a non-zero value\n",
		        kSpecCtrlPolicy, kSpecCtrlValue, confirmed, kCPUCount);
	else
		sprintf(line, "SPEC_CTRL: %s (0x%lx) on %u of %u threads; %u inherited non-zero (first: CPU %u, APIC %u = 0x%lx)\n",
		        kSpecCtrlPolicy, kSpecCtrlValue, confirmed, kCPUCount, nonzero, first, kCPUs[first].apic.id, firstVal);
	printf("%s", line);
	printd(DEBUG_BOOT, "%s", line);
}

// Interrupt handler for initializing an AP
void ap_initialization_handler() {
    
    // Which CPU this is. The BSP calls this directly, before it has a CLS;
    // an AP arrives by IPI with its CLS already published by ap_wakeup_entry.
    // cpu_self answers both.
    uint32_t cpu = cpu_self();
    printd(DEBUG_SMP,"AP%u: initialization handler\n",cpu);

    // NOTE: Stack was already initialized in ap_wakeup_entry

	//STAR MSR — segment selectors for the SYSCALL/SYSRET fast path.
	//Format: 63..48                 | 47..32                  | 31..0
	//--------|------------------------|-------------------------|------
	//        | SYSRET base selector   | SYSCALL base selector   | legacy 32-bit SYSCALL EIP (unused in long mode)
	//
	//SYSCALL loads CS = STAR[47:32] and SS = STAR[47:32]+8, so the base is simply
	//the kernel code selector (kernel data sits right after it in the GDT).
	//
	//SYSRETQ loads CS = STAR[63:48]+16 and SS = STAR[63:48]+8, so the base must be
	//8 BELOW the user data selector, with user code 8 above user data (the GDT is
	//laid out to satisfy this — see gdt.h).  RPL 3 is baked into the base so the
	//loaded selectors carry ring 3.  Both layout preconditions are asserted below
	//so a GDT reshuffle fails the build instead of #GP'ing on the first sysret.
	_Static_assert(GDT_KERNEL_DATA_ENTRY == GDT_KERNEL_CODE_ENTRY + 1,
	               "SYSCALL requires kernel data GDT entry immediately after kernel code");
	_Static_assert(GDT_USER_CODE_ENTRY == GDT_USER_DATA_ENTRY + 1,
	               "SYSRETQ requires user code GDT entry immediately after user data");

	uint64_t syscallBase = (uint64_t)GDT_KERNEL_CODE_ENTRY << 3;
	uint64_t sysretBase  = ((uint64_t)(GDT_USER_DATA_ENTRY - 1) << 3) | 3;
	uint64_t starValue   = (sysretBase << 48) | (syscallBase << 32);

    wrmsr64(STAR_MSR,starValue);   //SYSCALL: CS=0x28/SS=0x30; SYSRETQ: CS=0x43/SS=0x3B (RPL 3)

	//EFER.SCE — actually ENABLE the SYSCALL/SYSRET instructions.  The boot
	//environment leaves EFER with LME/LMA/NXE only; without SCE, `syscall`
	//raises #UD no matter how carefully STAR/LSTAR/SFMASK were programmed.
	//Per-core like the rest of this block (EFER is a per-core MSR).
	//
	//EFER.NXE joins it (2026-08-16, the NX arc): Limine does set NXE per its
	//spec, but ring-3 no-execute now DEPENDS on that bit — page-table bit 63
	//is RESERVED when NXE is off, so a single NX PTE under NXE=0 is a
	//reserved-bit #PF on the next access. A guarantee this load-bearing gets
	//set by the party relying on it, not inherited as a bootloader's habit.
	//CPUID-gated (0x80000001 EDX bit 20): a CPU without NX gets no NXE and,
	//by the same gate in paging_map_page's callers... no NX bits either —
	//every post-2003 x86-64 has it, but the gate costs three lines.
	{
		uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
		uint64_t efer = rdmsr64(EFER_MSR) | EFER_SCE;
		if (__get_cpuid(0x80000001, &eax, &ebx, &ecx, &edx) && (edx & (1u << 20)))
			efer |= EFER_NXE;
		wrmsr64(EFER_MSR, efer);
	}

	//LSTAR MSR
	//Format: 63..0 = Entry point to the kernel's system call method
	wrmsr64(LSTAR_MSR, (uintptr_t)&syscall_Enter);

	// SFMASK MSR — WHAT RING 3 IS NOT ALLOWED TO LEND US.
	//
	// Layout: bits are RFLAGS bits. A 1 here forces that flag to 0 on SYSCALL
	// entry. Everything NOT named here crosses the ring boundary into the
	// kernel exactly as ring 3 left it, and this line used to end with the
	// sentence "No need to touch other flags" — which is the whole reason
	// this comment exists now.
	//
	// THE PRINCIPLE, and it is not "because a spec says so": the kernel
	// should not inherit CPU state from the program it is protecting itself
	// from, whenever that state changes what the kernel's own instructions
	// MEAN. A flag ring 3 can set with one unprivileged instruction, that
	// silently alters the behaviour of code the kernel already compiled and
	// tested, is a hole with no local defence — you cannot review your way
	// out of it, because every individual line still looks correct.
	//
	// By that test, five flags earn a bit here (2026-08-24 — the two
	// originals plus three; this line said "four" until rd27 miscounted its
	// own list back at it):
	//
	//   IF (9)  — the kernel decides its own preemptibility. Original.
	//   TF (8)  — single-stepping is not ring 3's to impose on ring 0. Original.
	//   DF (10) — THE DIRECTION FLAG DECIDES WHICH WAY EVERY STRING COPY RUNS.
	//             With it set, a `rep movsq` walks DOWNWARD from its start,
	//             writing over what precedes the destination instead of what
	//             follows. The kernel binary carries twelve of them (GCC's
	//             inlined struct copies, present even at -O0) and not one has
	//             a `cld` in front of it, because the compiler is entitled to
	//             assume the flag is clear — which is exactly why an inherited
	//             DF is invisible to inspection. Cost of closing it: one bit.
	//             (Honest note: an attempt to demonstrate corruption through
	//             /proc maps did NOT reproduce — see df_test. The mechanism is
	//             certain, the exploit was not shown. Closed on the principle
	//             above, not on a proof-of-concept.)
	//   NT (14) — decides what IRET *IS*. With NT set, IRET means "return from
	//             a nested task" rather than "return from an interrupt", and
	//             long mode has no task switching, so it is a #GP instead. Same
	//             shape of hazard as DF: one ring-3 bit, one kernel instruction
	//             quietly redefined.
	//   AC (18) — inert today (alignment checking only applies at CPL 3, and
	//             SMAP is not enabled), so this one is bought forward rather
	//             than needed now. It is the bit that would let ring-3 state
	//             influence whether the kernel may touch user pages the day
	//             SMAP is turned on, and finding that out then, through a
	//             fault nobody can place, is worth avoiding for free today.
	//
	// The arithmetic flags are deliberately left alone: the kernel sets its
	// own before it tests them, so inheriting CF or ZF changes nothing. Bits
	// get masked here because they are load-bearing, not to be thorough.
	//
	// The interrupt and exception gates need the same treatment for DF and no
	// MSR covers them — they get an explicit `cld` in their entry stubs: one
	// at exc_common serves all 32 exception vectors; the device IRQs and the
	// IPI stubs in handle_mp_isr.S have their own; and — since rd27, a P1 —
	// so do the two entries the audit MISSED because they are not wired
	// through handle_mp_isr.S at all: `_schedule_ap` in scheduler.S (both
	// scheduler vectors point straight at it — the most-taken interrupt entry
	// in the kernel) and `nmi_handler` in handler_errors.S. The lesson is the
	// same as rd11's: an audit scoped to one file is not an audit. The IDT
	// table in idt.c is the list of entries; check THAT, not the directory.
	//
	// Found while fixing the narrower ring-3 half of the same disease: a
	// signal handler entered with DF set (Codex #29 rd10, signals.h
	// SIGNAL_RFLAGS_DF). That one needed hand-written ring-3 asm to reach.
	// This one needs a `std` and any syscall at all.
	wrmsr64(SFMASK_MSR, (1 << 18) | (1 << 14) | (1 << 10) | (1 << 9) | (1 << 8));

	// The speculation controls are this thread's and the kernel's to set —
	// never inherited (the SPEC_CTRL block above has the day it cost).
	spec_ctrl_init_this_cpu(cpu);

	init_core_local_storage(cpu);
	core_local_storage_t *cls = get_core_local_storage();

	// Calibrate the LAPIC timer frequency ONCE, on the BSP; APs inherit its
	// number. Every core on the package runs off the same crystal, so per-AP
	// recalibration was pure redundancy — and actively harmful: each AP spent
	// 3×10 tick-paced spins calibrating against a tick clock that AP bring-up
	// itself was starving (host-descheduled hot-spinning vCPUs), which BOTH
	// stretched boot (the 54-second VBox mystery, 2026-07-11) AND produced
	// 2x-wrong AP frequencies (the corrupted apicTPS readings). The BSP's
	// number is measured early, before any of that noise exists.
	if (cpu == BSP_CPU)
		mp_determine_local_APIC_timer_speed();
	else
		cls->apicTicksPerSecond =
			get_core_local_storage_for_core(BSP_CPU)->apicTicksPerSecond;

	//Divide the # of APIC timer ticks per second by the number of scheduler runs expected per second to get the timer's initial value
	cls->apicTimerCount = cls->apicTicksPerSecond / MP_SCHEDULER_RUNS_PER_SECOND;
    
	cls->coreInitialized = true;

	// Acknowledge the interrupt if not the BSP (BSP calls this method directly)
	if (cls->cpu != BSP_CPU)
    	write_eoi();  // Function to send an End-of-Interrupt signal to the APIC
}

void mp_restart_apic_timer_count()
{
	core_local_storage_t *cls = get_core_local_storage();
    // We need to write the count to the timer, but first get the current state of the LVT_TIMER register so we can restore it after
    // That way if the timer was disabled, it will remain disabled, and if it was enabled, it will remain enabled
    write_apic_register(kMPApicBase + APIC_TIMER_INIT_COUNT, cls->apicTimerCount);  //Trigger X times per second based on config setting
	//_write_eoi();
    //printd(DEBUG_SMP, "AP: restart_apic_timer_count: Timer is restarted (0x%08x)\n", val);
}

void ap_configure_scheduler_timer()
{
	core_local_storage_t *cls = get_core_local_storage();
    //NOTE: Configured to be disabled
    // Divide configuration: divisor = 16, matching the divisor the BSP's
    // calibration measured under (ap_get_timer_ticks_per_interval writes the
    // same 0x3 before counting). THIS WRITE WAS COMMENTED OUT until
    // 2026-08-13, and since only the BSP calibrates (7/11 change) every AP's
    // divide register sat at its reset default while consuming a count
    // denominated in divide-by-16 units — AP timers ran ~16x fast. Periodic
    // mode carried that silently (~1600Hz AP passes instead of 100); the
    // backstop's first flight measured it (152 lease expiries in 500ms where
    // 50ms leases predict 10) and this line came back from the dead. The
    // counts are core-local state but the crystal is shared: every core must
    // interpret the BSP's ticks-per-second under the BSP's divisor.
    write_apic_register(kMPApicBase + APIC_TIMER_DIVIDE_CONFIG, 0x3);
    uint32_t lvtValue;

    // Set the interrupt vector to 0x7E
    lvtValue = IPI_TIMER_SCHEDULE_VECTOR;

    // Set to periodic mode by setting the periodic mode bit
    lvtValue |= (1U << APIC_TIMER_PERIODIC_MODE_BIT);

    // Ensure the timer is masked (disabled) by setting the mask bit
    lvtValue = DISABLE_TIMER(lvtValue);
    // Write the configuration to the APIC LVT Timer Register
    write_apic_register(kMPApicBase + APIC_LVT_TIMER, lvtValue);
    
    //NOTE: localAPICTimerSpeed is how many times the local APIC timer ticks in 1 second
    write_apic_register(kMPApicBase + APIC_TIMER_INIT_COUNT, cls->apicTimerCount);  //Trigger X times per second based on config setting
    printd (DEBUG_SMP, "AP: ap_configure_scheduler_timer: Timer is configured (0x%08x) to fire INT 0x%02x every %u ticks (ticks per second=%u)\n", 
        lvtValue, 
        IPI_TIMER_SCHEDULE_VECTOR, 
        cls->apicTimerCount, 
        cls->apicTicksPerSecond);
    write_eoi();
}

void testAPTimerTickISR()
{
    printd(DEBUG_SMP, "AP: In testAPTimerTickISR at %u ticks\n", kTicksSinceStart);
    write_eoi();
}

// ── The tickless preemption lease (2026-08-13) ──────────────────────────────
// Under tickless the AP LAPIC timer is not a clock, it is a DEADLINE: every
// non-idle dispatch arms it one-shot for SCHED_BACKSTOP_MS, and an idle
// dispatch stops it cold. A well-behaved thread re-enters the scheduler
// (syscall, block, yield) before the lease expires and the next dispatch
// re-grants it; a syscall-free hog gets exactly one interrupt, at expiry,
// which runs the pass that preempts it. There is no tick — an idle core
// takes ZERO timer interrupts, which is the whole point of tickless and the
// property periodic mode can never have. Both functions run on the core
// they arm (LAPIC registers are core-local); the callers are scheduler_do's
// dispatch tail and nobody else.
void sched_backstop_arm(core_local_storage_t *cls)
{
    // Divide config FIRST, every arm: apicTicksPerSecond is denominated in
    // divide-by-16 counts (calibration writes 0x3 before measuring), so the
    // consuming timer must divide by 16 too. This cannot ride bring-up: a
    // tickless AP never receives IPI_ENABLE_SCHEDULING_VECTOR, so
    // ap_configure_scheduler_timer never runs here and the reset-default
    // divisor would eat the count ~16x fast — measured, not theorized: the
    // backstop's first flight fired 152 "50ms" leases in 500ms. Per-arm is
    // one cheap MMIO write and self-heals anything else that touches DCR.
    write_apic_register(kMPApicBase + APIC_TIMER_DIVIDE_CONFIG, 0x3);
    // One-shot: vector alone — mode bits clear, mask clear. The count write
    // stays LAST — it is THE arming action (same SDM discipline
    // enableAPScheduling_ISR documents: LVT writes may disarm, so arm after
    // the mode is final).
    uint32_t lvt = IPI_TIMER_SCHEDULE_VECTOR;   // vector, one-shot, unmasked
    write_apic_register(kMPApicBase + APIC_LVT_TIMER, lvt);
    // kSchedBackstopMS, not the constant: BACKSTOP=<ms> sets boot policy
    // (validated 1..1000 in kernel_commandline.c — never 0, which would
    // SDM-stop the timer and silently resurrect the starvation bug).
    write_apic_register(kMPApicBase + APIC_TIMER_INIT_COUNT,
                        (uint32_t)((cls->apicTicksPerSecond * (uint64_t)kSchedBackstopMS) / 1000));
}

void sched_backstop_disarm(void)
{
    // SDM 11.5.4: writing 0 to the initial-count register stops the timer.
    // The LVT can stay unmasked — a stopped timer fires nothing, and the next
    // arm rewrites the LVT anyway. A fire that races this disarm delivers one
    // spurious pass to an idle core, which the pass handles like any nudge.
    write_apic_register(kMPApicBase + APIC_TIMER_INIT_COUNT, 0);
}

void enableAPScheduling_ISR()
{
	ap_configure_scheduler_timer();
	core_local_storage_t *cls = get_core_local_storage();
	if (kTicklessScheduler && cls->cpu != BSP_CPU)
	{
		printd(DEBUG_SMP, "AP: enableAPScheduling_ISR: tickless mode active, leaving AP%lu timer masked\n", cls->cpu);
		write_eoi();
		return;
	}
    uint32_t val = read_apic_register(kMPApicBase + APIC_LVT_TIMER); // Use the read function
    //ConfigureAPITimer disables the timer, so enable it now
    val |= (1U << APIC_TIMER_PERIODIC_MODE_BIT);  // Ensure periodic mode is set
    val = ENABLE_TIMER(val);
    write_apic_register(kMPApicBase + APIC_LVT_TIMER, val);
    // Re-arm AFTER the final LVT write: the SDM permits LVT timer writes to
    // disarm the timer (and requires it for mode changes), and virtual APIC
    // implementations vary in how liberally they disarm. Writing the initial
    // count is THE arming action, so doing it last guarantees a running
    // timer on every implementation — on a lenient one (QEMU) it's just a
    // harmless phase reset.
    write_apic_register(kMPApicBase + APIC_TIMER_INIT_COUNT, cls->apicTimerCount);
    printd (DEBUG_SMP, "AP: enableAPScheduling_ISR: Timer is enabled (0x%08x)\n", val);
    write_eoi();
}

void disableAPScheduling_ISR()
{
    ap_configure_scheduler_timer();
    uint32_t val = read_apic_register(kMPApicBase + APIC_LVT_TIMER); // Use the read function
    //ap_configure_scheduler_timer configures the timer to be disabled
    printd (DEBUG_SMP, "AP: disableAPScheduling: Timer is disabled (0x%08x)\n", val);
    write_eoi();
}

// Enable the AP to do scheduling
void mp_enable_scheduling_vector(uint32_t cpu)
{
    // Enable the AP from the BSP
    send_ipi(cpu, IPI_ENABLE_SCHEDULING_VECTOR, 0, 1, 0);
    printd (DEBUG_SMP, "MP: mp_enable_scheduling_vector: IPI sent to AP%u for vector 0x%02x\n", cpu, IPI_ENABLE_SCHEDULING_VECTOR);
}

void mpEnableAP(uint32_t cpu)
{
    // Enable the AP from the AP
    send_ipi(cpu, IPI_ENABLE_SCHEDULING_VECTOR, 0, 1, 0);
    printd (DEBUG_SMP, "MP: mpEnableAP: AP%u, IPI sent for vector 0x%02x\n", cpu, IPI_ENABLE_SCHEDULING_VECTOR);
}

void mpDisableAP(uint32_t cpu)
{
    // Disable the AP from the AP
    send_ipi(cpu, IPI_DISABLE_SCHEDULING_VECTOR, 0, 1, 0);
    printd (DEBUG_SMP, "MP: mpDisableAP: AP%u, IPI sent for vector 0x%02x\n", cpu, IPI_DISABLE_SCHEDULING_VECTOR);
}

// ── CPU-time settle-on-read ─────────────────────────────────────────────
// The accounting charges at context-switch boundaries, which means a
// core's books are only as fresh as its last scheduler pass — and a
// monopolized or rarely-nudged core (tickless APs) settles in LUMPS: the
// top -l audit measured idle1 alternating 0% and 208% per refresh while
// summing to a perfect 0.9984 over time. Correct books, quantized
// delivery. The fix: before /proc renders CPU-time numbers, ask every
// core to settle its own in-flight span — each using ITS OWN TSC (the
// reader must never do cross-core TSC math; that is the desync landmine).
// Mode-agnostic by construction (Chris's requirement): an IPI lands the
// same under tickless or periodic, and the handler is core-local
// either way.

volatile bool mp_acctSettleAck[MAX_CPUS];
extern volatile uint64_t kTicksSinceStart;


// The compositor's hlt-wait bookends (smp.h has the field doctrine).
//
// Begin: called UNDER CLI, immediately before sti;hlt — settle first, so
// everything up to this instant (the real compositing work) lands on the
// caller, THEN raise the flag so the nap ahead lands on idle.
//
// End: called after hlt returns, interrupts live again — wrapped in its own
// cli window. An ordinary IRQ or scheduler dispatch has already closed the
// nap and cleared the redirect, so this settles the caller's post-wake tail.
// If another wake source left the redirect up, End closes it here.
void mpAcctHaltBegin(void)
{
    accounting_halt_begin();
}

void mpAcctHaltEnd(void)
{
    accounting_halt_end();
}

void acct_settle_ISR(void)
{
    core_local_storage_t *cls = get_core_local_storage();
    // The scheduler owns its open span between its entry and exit charges.
    // Its short prologue/epilogue windows still belong to the pass itself:
    // do not settle there, where the saved scheduler frame may name a
    // different thread. IRQ entry already closed the interrupted mode;
    // an active pass can settle into acctSchedCycles.
    if (cls != NULL && (!mp_inScheduler[cls->cpu] || cls->acctSchedSpanActive))
        accounting_charge(0);
    if (cls != NULL)
        mp_acctSettleAck[cls->cpu] = true;
    write_eoi();
}

void mpAcctSettleAll(void)
{
    // Rate limit: books fresher than one tick are fresh enough. A top
    // refresh reads ~30 /proc files back to back; only the first pays
    // for the broadcast, the rest reuse the same settle.
    static volatile uint64_t lastSettleTick = (uint64_t)-1;
    if (kTicksSinceStart == lastSettleTick)
        return;
    lastSettleTick = kTicksSinceStart;

    // Local core first. The charge routine masks interrupts around its
    // tuple update; the same scheduler-phase rule applies here and in the
    // remote IPI handler.
    // The ack policy below needs the caller's IF, which accounting_charge
    // preserves across its own interrupt-masked update.
    uint64_t flags;
    __asm__ volatile("pushfq\n\tpop %0" : "=r"(flags) :: "memory");
    core_local_storage_t *local = get_core_local_storage();
    if (local != NULL && (!mp_inScheduler[local->cpu] || local->acctSchedSpanActive))
        accounting_charge(0);

    if (!kSMPInitDone)
        return;

    kDiagSettleBroadcasts++;                            // TEMP DIAG
    uint32_t self = (uint32_t)get_core_local_storage()->cpu;
    for (uint32_t cpu = 0; cpu < kCPUCount; cpu++)
    {
        if (cpu == self)
            continue;
        mp_acctSettleAck[cpu] = false;
        send_ipi(cpu, IPI_ACCT_SETTLE_VECTOR, 0, 1, 0);
    }

    // NEVER WAIT FOR AN INTERRUPT-DELIVERED ANSWER WITH INTERRUPTS OFF.
    // (2026-08-10, diagnosed from a guest Chris paused mid-freeze — four cores,
    // every one accounted for.)
    //
    // The ack below can only arrive by the target SERVICING acct_settle_ISR.
    // If we spin for it with IF=0, we have asked for a message we have
    // forbidden ourselves to receive. And every caller of this function is in
    // procfs — i.e. inside a syscall — where SFMASK (smp_core.c ~line 461,
    // bit 9 = IF) masks interrupts for the WHOLE syscall. So the wait was
    // ALWAYS running interrupts-off.
    //
    // With two cores reading /proc at once (one `top` refresh overlapping
    // another, which is the normal case, not the rare one) it is a textbook
    // mutual deadlock: core A clears B's ack and spins, core B clears A's ack
    // and spins, and NEITHER can take the other's IPI. The frozen guest showed
    // it exactly — CPU0 and CPU3 both parked at this loop with IF=0,
    // mp_acctSettleAck[0] and [3] both false, each waiting on the other. The
    // machine only recovered because the spin was BOUNDED: ~10-20 seconds of
    // every core wedged (CPU1 and CPU2 piled up behind the scheduler lock),
    // and then life resumed as if nothing happened. The bound was the only
    // thing standing between this and a dead OS.
    //
    // The fix is not a bigger bound or a cleverer wait — it is to notice that
    // this function ALREADY documents the correct fallback: "a wedged core
    // must cost us staleness, never a hang." So when we cannot receive the
    // ack, we do not ask for it. The IPIs above still went out; the remote
    // cores still settle as soon as they next take an interrupt; the reader
    // just renders books that are one settle-round old — which is precisely
    // the freshness proc_gen_cores already accepts for its other fields
    // ("worst case one slice stale", procfs.c).
    //
    // `flags` is the caller's RFLAGS captured before the local charge.
    if (!(flags & 0x200))
        return;                                         // IF was off — send-and-go

    // Interrupts were on, so an ack can actually reach us: bounded wait, and
    // the bound is a SAFETY NET, not a budget. A core that answers at all
    // answers in microseconds; anything longer means it is busy with IF=0
    // (a syscall of its own), and waiting out its syscall buys staleness we
    // already said we would rather have. The old 2,000,000 was sized as if
    // spinning were free — under an emulator that traps pause loops it is
    // tens of seconds.
    for (uint32_t cpu = 0; cpu < kCPUCount; cpu++)
    {
        if (cpu == self)
            continue;
        uint64_t spins = 0;
        while (!mp_acctSettleAck[cpu] && ++spins < 50000UL)
            __builtin_ia32_pause();
        if (spins > kDiagSettleMaxSpins)                // TEMP DIAG
            kDiagSettleMaxSpins = spins;                // TEMP DIAG
        if (!mp_acctSettleAck[cpu])                     // TEMP DIAG: bound hit
        {
            kDiagSettleTimeouts++;                      // TEMP DIAG
            kDiagSettleLastLateCPU = cpu;               // TEMP DIAG
        }
    }
}

void mpSendInvTLB()
{
    // Until the APs are actually up there is nobody to shoot down — and
    // sending IPIs at parked/uninitialized cores is asking for trouble.
    if (!kSMPInitDone)
        return;
    // Send an IPI to the AP to invalidate the TLB
    // Send to all APs except the current one
    uint32_t self = cpu_self();
    for (uint32_t cpu = 0; cpu < kCPUCount; cpu++) {
        if (cpu != self) {
            send_ipi(cpu, IPI_INVALIDATE_TLB_VECTOR, 0, 1, 0);
            printd (DEBUG_SMP, "MP: mpSendInvTLB: AP%u, IPI sent for vector 0x%02x\n", cpu, IPI_INVALIDATE_TLB_VECTOR);
        }
    }
}

void enableApicTimerInterrupt() {
    uint32_t val = read_apic_register(APIC_LVT_TIMER); // Use the read function
    val &= ~(1U << APIC_LVT_MASK_BIT);  // Clear the mask bit to enable
    write_apic_register(APIC_LVT_TIMER, val);
}

void disableApicTimerInterrupt() {
    uint32_t val = read_apic_register(APIC_LVT_TIMER); // Use the read function
    val |= (1U << APIC_LVT_MASK_BIT);  // Set the mask bit to disable
    write_apic_register(APIC_LVT_TIMER, val);
}

// The freeze IPI's C half. Acknowledges the interrupt so the LAPIC's ISR bit
// does not stay set (a stuck ISR bit is its own catastrophe — see the IRQ0
// headstone in smp_core.h), then returns to the stub's hlt loop and never
// leaves it. Nothing else: a core about to stop forever must not take a lock,
// touch the scheduler, or print — the core that sent this is mid-report and
// owns the wire.
void freeze_core_ISR(void)
{
    write_eoi();
}

// Stop every OTHER core, permanently. Called by a core that is about to print
// a fatal report and halt: without it the report is a race against whatever
// those cores repaint next, and on hardware with no serial capture the screen
// IS the record. Fire-and-forget by design — there is no acknowledgement to
// wait for, and a core too wedged to take the IPI was never going to overwrite
// anything anyway.
void mpFreezeOtherCores(void)
{
    if (!kSMPInitDone)
        return;
    uint32_t self = cpu_self();
    for (uint32_t cpu = 0; cpu < kCPUCount; cpu++) {
        if (cpu != self)
            send_ipi(cpu, IPI_FREEZE_VECTOR, 0, 1, 0);
    }
}

void inv_tlb_ISR()
{
    
    printd(DEBUG_SMP,"Flush\n");
        __asm__("mov rax,cr3\ncmp rax,0\nje overflush\nmov cr3,rax\noverflush:\n");
        write_eoi();
}
