#include <stdint.h>
#include <stddef.h>
#include "smp_core.h"
#include "smp_offsets.h"
#include "x86_64.h"
#include "memory/paging.h"   // pat_init_this_core — per-core WC PAT entry
#include "CONFIG.h"
#include "msr.h"
#include "smp.h"
#include "gdt.h"
#include <stdint.h>
#include "serial_logging.h"
#include "kmalloc.h"
#include "kernel.h"
#include "time.h"
#include "apic.h"
#include "gdt.h"
#include "tss.h"
#include "thread.h"
#include "idt.h"
#include "panic.h"

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
volatile uint64_t kDiagSettleLastLateAPIC = 0;
// Sampled by every scheduler pass ON ITS OWN CORE (LAPIC is core-local):
// which vector is actually in service (names the interrupt that drove the
// pass — timer 0x7E vs manual 0x81 vs something unexpected), and the LVT
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
uintptr_t stackVirtualAddress, stackPhysicalAddress;
uintptr_t kMPEOIOffset = 0;
// The AP bootstrap stack. ONE, shared — safe because ap_wake_up_aps brings
// APs through strictly one at a time (the coreInitialized handshake below).
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
uint32_t temp_apic_id;
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

void send_ipi(uint32_t apic_id, uint32_t vector, uint32_t delivery_mode, uint32_t level, uint32_t trigger_mode) 
{
	core_local_storage_t *target_cls = get_core_local_storage_for_core(apic_id);
	core_local_storage_t *sender_cls = get_core_local_storage();
	uint32_t sender_apic_id = sender_cls ? sender_cls->apic_id : BOOTSTRAP_PROCESSOR_ID;
	if (mp_inScheduler[target_cls->apic_id] && vector == IPI_MANUAL_SCHEDULE_VECTOR)
	{
		printd(DEBUG_SMP | DEBUG_DETAILED,"MP: send_ipi_int - NOT sending an scheduling IPI because we're already in the scheduler");
		return;
	}
    printd(DEBUG_SMP | DEBUG_DETAILED,"MP: AP%u sending IPI 0x%02x to AP%u\n",sender_apic_id, vector, apic_id);
    // Ensure previous IPI command has completed — with a bound. An IPI whose
    // delivery-status bit never clears (wedged target core, virtual APIC
    // quirk) previously spun here forever, silently; a loud panic with the
    // vector and sender beats an invisible hang every time.
    uint64_t icr_wait_spins = 0;
    while (*((volatile uint32_t*)(kMPICRLow)) & 0x01000)
    {
        if (++icr_wait_spins > 100000000UL)
            panic("send_ipi: ICR delivery-status stuck busy — AP%u sending vector 0x%02x to AP%u never completed", sender_apic_id, vector, apic_id);
        __builtin_ia32_pause();
    }
    kDiagIPISends++;                                   // TEMP DIAG
    if (icr_wait_spins > kDiagICRMaxSpins)             // TEMP DIAG
        kDiagICRMaxSpins = icr_wait_spins;             // TEMP DIAG

    // Write to the high pa Canrt of the ICR (destination field)
    *((volatile uint32_t*)(kMPICRHigh)) = apic_id << 24;

    // Write to the low part of the ICR (command and vector)
	// Removed setting of level bit 14 (| (level << 14) )
    uint32_t icr_low_value = vector | (delivery_mode << 8) | (level << 14) | (trigger_mode << 15) | 0x00004000;
    *((volatile uint32_t*)(kMPICRLow)) = icr_low_value;
    printd(DEBUG_SMP | DEBUG_DETAILED,"MP: AP%u programmed ICR for AP%u (0x%04x)\n",sender_apic_id, apic_id, vector);
}

static inline void set_gs_base(uint64_t base) {
	wrmsr64(IA32_GS_BASE, base);
	kCLSInitialized = true;
}

void init_core_local_storage(unsigned apic_id)
{

	uint64_t coreBase = (uint64_t)kCoreLocalStorage +
                        (apic_id * sizeof(core_local_storage_t));
    set_gs_base(coreBase);
	core_local_storage_t *cls = (core_local_storage_t*)coreBase;
	cls->apic_id = apic_id;
	cls->self = cls;
	cls->tss = tss_get_for_cpu(apic_id);
	cls->kernel_rsp0 = cls->tss ? cls->tss->rsp0 : 0;

	// Allocate upper-half kernel interrupt stack for CR3 switching
	void *kernel_stack = kmalloc_aligned(KERNEL_INTERRUPT_STACK_SIZE);
	if (!kernel_stack)
		panic("Failed to allocate kernel interrupt stack for CPU %u", apic_id);

	cls->kernel_interrupt_stack_base = (uintptr_t)kernel_stack;
	cls->kernel_interrupt_stack_top = (uintptr_t)kernel_stack + KERNEL_INTERRUPT_STACK_SIZE;

	kMPEOIOffset = kMPApicBase | APIC_EOI_OFFSET;
	printd(DEBUG_THREAD | DEBUG_DETAILED, "Core local storage initialized to 0%16lx for core %u (kernel stack: 0x%lx-0x%lx)\n",
		coreBase, apic_id, cls->kernel_interrupt_stack_base, cls->kernel_interrupt_stack_top);
}

// Called to finish initializing the AP (stack switch has been done in ap_wakeup_entry())
void ap_wakeup_after_stack_switch(uint64_t apic_id, uint64_t stackVirtualAddress, uint64_t stackPhysicalAddress)
{
    volatile core_local_storage_t *temp_cls = get_core_local_storage();
    temp_cls->stackVirtualAddress = stackVirtualAddress;
    temp_cls->stackPhysicalAddress = stackPhysicalAddress;

    printd(DEBUG_SMP, "AP%u: Initial stack v/p: 0x%016x/0x%016x\n",apic_id, stackVirtualAddress, stackPhysicalAddress);

    // Read APIC_BASE_MSR
    uint32_t lvt_timer = read_apic_register(kMPApicBase + APIC_LVT_TIMER);
    uint32_t spurious_vector = read_apic_register(kMPApicBase + APIC_SPURIOUS_VECTOR);
    printd(DEBUG_SMP, "AP%u: LVT_TIMER = 0x%08x, SPURIOUS_VECTOR = 0x%08x\n", apic_id, lvt_timer, spurious_vector);

	*((volatile uint32_t*)(kMPICRHigh)) = apic_id << 24;  // Set destination APIC ID
	*((volatile uint32_t*) (kCPUInfo[apic_id].apic_tpr)) = 0x30;  // Correct TPR
	__asm__ volatile ("mfence");  // Ensure memory writes complete

	// PAT entry 7 = write-combining on THIS core, same as the BSP set before
	// the kernel tables were built (kernel.c) — IA32_PAT is per-core and the
	// SDM wants uniformity. Must happen before this core ever stores through
	// the framebuffer's PAGE_WC mapping (the compositor runs on APs).
	pat_init_this_core();

	*((volatile uint32_t*)kCPUInfo[apic_id].apic_svr) |= 0x100; // Set bit 8 (Enable LAPIC)
	//EOI to clear out the IRR as we don't know what is awaiting us when we enable the APIC/LVT otherwise
	*((volatile uint32_t*)kCPUInfo[apic_id].apic_eoi) = 0;

	// Debugging: Check if AP is ready to receive IPI
    printd(DEBUG_SMP | DEBUG_DETAILED, "AP%u: Ready to receive IPI? APIC_STATUS = 0x%08x\n", apic_id, *((volatile uint32_t*)kMPICRLow));

	// Set the spurious vector to 0xFF and enable interrupts (bit 8)
	*((volatile uint32_t*) (kCPUInfo[apic_id].apic_svr)) = 0x1FF;  // Enable APIC + Set spurious vector to 0xFF
	__asm__ volatile ("mfence");  // Ensure memory writes complete

	// Debugging: Confirm that AP is now ready to receive IPIs
	printd(DEBUG_SMP | DEBUG_DETAILED, "AP%u: Ready to receive IPI? APIC_STATUS = 0x%08x\n", apic_id, *((volatile uint32_t*)kMPICRLow));

	printd(DEBUG_SMP | DEBUG_DETAILED, "AP%u: LVT before: 0x%08x\n", apic_id, *((volatile uint32_t*)kMPLVTTimer));
	
	// Unmask LVT0 (timer) and LVT1 (error) by clearing the mask bit (bit 16)
	*((volatile uint32_t*)kMPLVTTimer) &= ~0x10000;  // Unmask LVT0 (timer)
	*((volatile uint32_t*)kMPLVTTimer) &= ~0x20000;  // Unmask LVT1 (error)
	__asm__ volatile ("mfence");  // Ensure memory writes complete
	// Debugging: Confirm LVT lines are unmasked
	printd(DEBUG_SMP | DEBUG_DETAILED, "AP%u: LVT after: 0x%08x\n", apic_id, *((volatile uint32_t*)kMPLVTTimer));

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

    temp_apic_id = read_apic_id();

    // Set up the rest of the AP initialization
    load_gdt_and_jump(&kGDTr);
    tss_initialize_cpu(temp_apic_id);
    asm volatile ("lidt %0" : : "m" (kIDTPtr));

	// Set up the AP stack
    stackVirtualAddress = (uintptr_t)kmalloc_aligned(AP_STACK_SIZE);
    stackPhysicalAddress = stackVirtualAddress & ~(kHHDMOffset);

    __asm__("mov rsp, %0\n"::"r" (stackVirtualAddress + AP_STACK_SIZE - sizeof(uintptr_t)));
	
    tss_set_rsp0(temp_apic_id, stackVirtualAddress + AP_STACK_SIZE - sizeof(uintptr_t));

    init_core_local_storage(temp_apic_id);

	tempCls = get_core_local_storage();
    if (tempCls)
    {
        tempCls->kernel_rsp0 = stackVirtualAddress + AP_STACK_SIZE - sizeof(uintptr_t);
    }

	// Initialize the AP after stack switch (set spurious vector, enable interrupts, etc.)
    ap_wakeup_after_stack_switch(temp_apic_id, stackVirtualAddress, stackPhysicalAddress);

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

	//TODO: Remve me!
	//Temporary debugging statement
	for (int coreToWake = 0; coreToWake < kMPCoreCount; coreToWake++) {
        uint32_t apic_id = kCPUInfo[coreToWake].apicID;
        if (apic_id == BOOTSTRAP_PROCESSOR_ID) continue; // Skip BSP
        
        printd(DEBUG_SMP, "MP: Waking up AP %u\n", apic_id);
			// BOOTMARK mile-markers (kernel.h) — permanent, BOOTMARKS-gated.
			// Per-AP resolution because AP bring-up was the 54-second culprit.
			if (kEnableBootmarks)
				printf("BOOTMARK ap%u-wake              tick=%lu tsc=%lu\n", apic_id, kTicksSinceStart, rdtsc());
			*((volatile uint64_t *) kCPUInfo[apic_id].goto_address) = (uint64_t) &ap_wakeup_entry;

			cls = get_core_local_storage_for_core(coreToWake);

			while (!cls->coreAwoken) {wait(10);}
			if (kEnableBootmarks)
				printf("BOOTMARK ap%u-awoken            tick=%lu tsc=%lu\n", apic_id, kTicksSinceStart, rdtsc());
			send_ipi(apic_id, IPI_AP_INITIALIZATION_VECTOR, 0, 1, 0);
			while (!cls->coreInitialized) {wait(10);}
			if (kEnableBootmarks)
				printf("BOOTMARK ap%u-initialized       tick=%lu tsc=%lu\n", apic_id, kTicksSinceStart, rdtsc());

			// The AP is up and off the bootstrap stack — verify it never
			// touched the floor. A smashed canary means bring-up came
			// within 64 bytes of repeating the /idle stray-write saga
			// (SCHEDULER_STRAY_WRITE.md), and that is a stop-the-line
			// event, not a log line: the .bss below may already be dirty.
			for (int q = 0; q < TEMP_STACK_CANARY_QWORDS; q++)
				if (((volatile uint64_t *)tempStack)[q] != TEMP_STACK_CANARY)
					panic("AP %u bring-up smashed the tempStack canary (qword %u = 0x%016lx) — grow TEMP_STACK_SIZE; see SCHEDULER_STRAY_WRITE.md\n",
						apic_id, q, ((volatile uint64_t *)tempStack)[q]);
			if (kTicklessScheduler)
			{
				// Tickless (wake-on-work): park AP timers and kick each AP once so it can run its idle thread.
				send_ipi(apic_id, IPI_MANUAL_SCHEDULE_VECTOR, 0, 1, 0);
			}
			else
			{
				send_ipi(apic_id, IPI_ENABLE_SCHEDULING_VECTOR, 0, 1, 0);
			}
	    }
		kSMPInitDone = true;
}

void ap_enable_schedulers() {
    for (int i = 0; i < kMPCoreCount; i++) {
        uint32_t apic_id = kCPUInfo[i].apicID;
        if (apic_id == BOOTSTRAP_PROCESSOR_ID) continue; // Skip BSP
        
        printd(DEBUG_SMP, "MP: Enabling scheduling on AP %u\n", apic_id);
        if (kTicklessScheduler)
            send_ipi(apic_id, IPI_MANUAL_SCHEDULE_VECTOR, 0, 1, 0);
        else
            mp_enable_scheduling_vector(apic_id);
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

// Interrupt handler for initializing an AP
void ap_initialization_handler() {
    
    // Get the APIC ID to identify the processor
    uint32_t apic_id = read_apic_id();  // Function to read the APIC ID register
    printd(DEBUG_SMP,"AP: initialization handler\n",apic_id);

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
	wrmsr64(EFER_MSR, rdmsr64(EFER_MSR) | EFER_SCE);

	//LSTAR MSR
	//Format: 63..0 = Entry point to the kernel's system call method
	wrmsr64(LSTAR_MSR, (uintptr_t)&syscall_Enter);

	//SFMASK MSR
	//Layout: Bits are the same as RFLAGS.  A 1 in a bit causes the associated RFLAG bit to be set to 0
	//We'll mask IF and TF so that they are set to 0.  No need to touch other flags.
	//On SYSCALL entry interrupts will be disabled, and the trap flag will not be set
	wrmsr64(SFMASK_MSR, (1 << 9) | (1 << 8));

	init_core_local_storage(apic_id);
	core_local_storage_t *cls = get_core_local_storage();

	// Calibrate the LAPIC timer frequency ONCE, on the BSP; APs inherit its
	// number. Every core on the package runs off the same crystal, so per-AP
	// recalibration was pure redundancy — and actively harmful: each AP spent
	// 3×10 tick-paced spins calibrating against a tick clock that AP bring-up
	// itself was starving (host-descheduled hot-spinning vCPUs), which BOTH
	// stretched boot (the 54-second VBox mystery, 2026-07-11) AND produced
	// 2x-wrong AP frequencies (the corrupted apicTPS readings). The BSP's
	// number is measured early, before any of that noise exists.
	if (apic_id == BOOTSTRAP_PROCESSOR_ID)
		mp_determine_local_APIC_timer_speed();
	else
		cls->apicTicksPerSecond =
			get_core_local_storage_for_core(BOOTSTRAP_PROCESSOR_ID)->apicTicksPerSecond;

	//Divide the # of APIC timer ticks per second by the number of scheduler runs expected per second to get the timer's initial value
	cls->apicTimerCount = cls->apicTicksPerSecond / MP_SCHEDULER_RUNS_PER_SECOND;
    
	cls->coreInitialized = true;

	// Acknowledge the interrupt if not the BSP (BSP calls this method directly)
	if (cls->apic_id != BOOTSTRAP_PROCESSOR_ID)
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
    // Set divide configuration to 1
	// Set divide configuration - divisor = 16, same as when we established the frequency earlier
    //write_apic_register(kMPApicBase + APIC_TIMER_DIVIDE_CONFIG, 0x3);
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

void enableAPScheduling_ISR()
{
	ap_configure_scheduler_timer();
	core_local_storage_t *cls = get_core_local_storage();
	if (kTicklessScheduler && cls->apic_id != BOOTSTRAP_PROCESSOR_ID)
	{
		printd(DEBUG_SMP, "AP: enableAPScheduling_ISR: tickless mode active, leaving AP %u timer masked\n", cls->apic_id);
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
void mp_enable_scheduling_vector(int apic_id)
{
    // Enable the AP from the BSP
    send_ipi(apic_id, IPI_ENABLE_SCHEDULING_VECTOR, 0, 1, 0);
    printd (DEBUG_SMP, "MP: mp_enable_scheduling_vector: IPI sent to APIC %u for vector 0x%02x\n", apic_id, IPI_ENABLE_SCHEDULING_VECTOR);
}

void mpEnableAP(int apic_id)
{
    // Enable the AP from the AP
    send_ipi(apic_id, IPI_ENABLE_SCHEDULING_VECTOR, 0, 1, 0);
    printd (DEBUG_SMP, "MP: mpEnableAP: APIC %u, IPI sent for vector 0x%02x\n", apic_id, IPI_ENABLE_SCHEDULING_VECTOR);
}

void mpDisableAP(int apic_id)
{
    // Disable the AP from the AP
    send_ipi(apic_id, IPI_DISABLE_SCHEDULING_VECTOR, 0, 1, 0);
    printd (DEBUG_SMP, "MP: mpDisableAP: APIC %u, IPI sent for vector 0x%02x\n", apic_id, IPI_DISABLE_SCHEDULING_VECTOR);
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


// The core-local settle: charge the running thread up to "now", restamp.
// Called from the IPI handler (interrupts off) and inline for the local
// core (under cli) — in both cases it cannot interleave with this core's
// own scheduler pass, which is the only other writer of these fields.
static void acct_settle_local(void)
{
    core_local_storage_t *cls = get_core_local_storage();
    if (cls == NULL)
        return;
    uint64_t now = rdtsc();
    if (cls->acctLastDispatchTSC != 0 &&
        cls->currentThread != NULL && cls->currentThread != NO_THREAD)
    {
        cls->currentThread->runCycles += now - cls->acctLastDispatchTSC;
        cls->acctLastDispatchTSC = now;
    }
}

void acct_settle_ISR(void)
{
    core_local_storage_t *cls = get_core_local_storage();
    // The pass owns the meter while it runs. scheduler_do charges the
    // outgoing thread at pass ENTRY but doesn't move acctLastDispatchTSC
    // until pass EXIT — and the pass runs with interrupts enabled, so this
    // IPI can land in between. Settling in that window re-charges the whole
    // [lastDispatch → now] span on top of the entry charge: on a monopolized
    // core that goes seconds between passes, that's seconds double-billed
    // (top -d 5000 showed hogs >100% exactly this way, P5 2026-07-30). One
    // settle of staleness beats double-charging — skip, ack, let the pass
    // close its own books. (The header's "cannot interleave with this core's
    // own scheduler pass" claim was simply wrong; this makes it true.)
    if (cls != NULL && !mp_inScheduler[cls->apic_id])
        acct_settle_local();
    if (cls != NULL)
        mp_acctSettleAck[cls->apic_id] = true;
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

    // Local core first, atomically vs our own interrupts.
    uint64_t flags;
    __asm__ volatile("pushfq\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");
    acct_settle_local();
    __asm__ volatile("push %0\n\tpopfq" :: "r"(flags) : "memory", "cc");

    if (!kSMPInitDone)
        return;

    kDiagSettleBroadcasts++;                            // TEMP DIAG
    uint32_t self = get_core_local_storage()->apic_id;
    for (int i = 0; i < kMPCoreCount; i++)
    {
        uint32_t apic_id = kCPUInfo[i].apicID;
        if (apic_id == self)
            continue;
        mp_acctSettleAck[apic_id] = false;
        send_ipi(apic_id, IPI_ACCT_SETTLE_VECTOR, 0, 1, 0);
    }

    // Bounded wait for the acks: a wedged core must cost us staleness,
    // never a hang — its books just stay one settle behind, which is
    // exactly the pre-IPI status quo for that core.
    for (int i = 0; i < kMPCoreCount; i++)
    {
        uint32_t apic_id = kCPUInfo[i].apicID;
        if (apic_id == self)
            continue;
        uint64_t spins = 0;
        while (!mp_acctSettleAck[apic_id] && ++spins < 2000000UL)
            __builtin_ia32_pause();
        if (spins > kDiagSettleMaxSpins)                // TEMP DIAG
            kDiagSettleMaxSpins = spins;                // TEMP DIAG
        if (!mp_acctSettleAck[apic_id])                 // TEMP DIAG: bound hit
        {
            kDiagSettleTimeouts++;                      // TEMP DIAG
            kDiagSettleLastLateAPIC = apic_id;          // TEMP DIAG
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
    for (int i = 0; i < kMPCoreCount; i++) {
        uint32_t apic_id = kCPUInfo[i].apicID;
        if (apic_id != read_apic_id()) {
            send_ipi(apic_id, IPI_INVALIDATE_TLB_VECTOR, 0, 1, 0);
            printd (DEBUG_SMP, "MP: mpSendInvTLB: APIC %u, IPI sent for vector 0x%02x\n", apic_id, IPI_INVALIDATE_TLB_VECTOR);
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

void inv_tlb_ISR()
{
    
    printd(DEBUG_SMP,"Flush\n");
        __asm__("mov rax,cr3\ncmp rax,0\nje overflush\nmov cr3,rax\noverflush:\n");
        write_eoi();
}
