#include <stdint.h>
#include "smp_core.h"
#include "x86_64.h"
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

extern struct IDTPointer kIDTPtr;
extern void syscall_Enter();
extern volatile bool mp_inScheduler[MAX_CPUS];
extern void ap_wakeup_stub();
extern uint64_t kKernelPML4;
extern uint64_t kKernelPML4v;
extern uint64_t kHHDMOffset;
extern uint64_t kMPLVTTimer;
bool kCLSInitialized = false;
bool kSMPInitDone = false;
uintptr_t stackVirtualAddress, stackPhysicalAddress;
uintptr_t kMPEOIOffset = 0;
uint8_t tempStack[1024];
uint32_t temp_apic_id;
core_local_storage_t *tempCls;

// Assuming kMPApicBase and APIC_EOI_OFFSET are properly defined elsewhere
extern volatile uintptr_t kMPApicBase;
extern void _write_eoi();

#define APIC_EOI_OFFSET 0xB0

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
	core_local_storage_t *cls = get_core_local_storage_for_core(apic_id);
	if (mp_inScheduler[cls->apic_id] && vector == IPI_MANUAL_SCHEDULE_VECTOR)
	{
		printd(DEBUG_SMP | DEBUG_DETAILED,"MP: send_ipi_int - NOT sending an scheduling IPI because we're already in the scheduler");
		return;
	}
    printd(DEBUG_SMP | DEBUG_DETAILED,"MP: Sending IPI for 0x%02x to AP%u\n",vector, apic_id);
    // Ensure previous IPI command has completed
    while (*((volatile uint32_t*)(kMPICRLow)) & 0x01000){};

    // Write to the high part of the ICR (destination field)
    *((volatile uint32_t*)(kMPICRHigh)) = apic_id << 24;

    // Write to the low part of the ICR (command and vector)
	// Removed setting of level bit 14 (| (level << 14) )
    uint32_t icr_low_value = vector | (delivery_mode << 8) | (level << 14) | (trigger_mode << 15) | 0x00004000;
    *((volatile uint32_t*)(kMPICRLow)) = icr_low_value;
    printd(DEBUG_SMP | DEBUG_DETAILED,"MP: IPI delivered\n",apic_id);
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
	kMPEOIOffset = kMPApicBase | APIC_EOI_OFFSET;
	printd(DEBUG_THREAD | DEBUG_DETAILED, "Core local storage initialized to 0%16lx for core %u\n", coreBase, apic_id);
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
        :: "r" (kKernelPML4), "r" ((uint16_t)0x30), "r" (tempStack + 1024 - 8)
    );

    temp_apic_id = read_apic_id();

    // Set up the rest of the AP initialization
    load_gdt_and_jump(&kGDTr);
    init_tss();
    asm volatile ("lidt %0" : : "m" (kIDTPtr));

	// Set up the AP stack
    stackVirtualAddress = (uintptr_t)kmalloc_aligned(AP_STACK_SIZE);
    stackPhysicalAddress = stackVirtualAddress & ~(kHHDMOffset);

    __asm__("mov rsp, %0\n"::"r" (stackVirtualAddress + AP_STACK_SIZE - sizeof(uintptr_t)));
	
    init_core_local_storage(temp_apic_id);

	tempCls = get_core_local_storage();

	// Initialize the AP after stack switch (set spurious vector, enable interrupts, etc.)
    ap_wakeup_after_stack_switch(temp_apic_id, stackVirtualAddress, stackPhysicalAddress);

    // Loop to ensure the AP doesn't fall off the function
    while (1) {
        __asm__("sti\nhlt\n");  // Enable interrupts and halt the AP
    }
}
void ap_wake_up_aps() {
	volatile core_local_storage_t *cls;
    
	//TODO: Remve me!
	//Temporary debugging statement
	for (int coreToWake = 0; coreToWake < kMPCoreCount; coreToWake++) {
        uint32_t apic_id = kCPUInfo[coreToWake].apicID;
        if (apic_id == BOOTSTRAP_PROCESSOR_ID) continue; // Skip BSP
        
        printd(DEBUG_SMP, "MP: Waking up AP %u\n", apic_id);
		*((volatile uint64_t *) kCPUInfo[apic_id].goto_address) = (uint64_t) &ap_wakeup_entry;

		cls = get_core_local_storage_for_core(coreToWake);

		while (!cls->coreAwoken) {wait(10);}
        send_ipi(apic_id, IPI_AP_INITIALIZATION_VECTOR, 0, 1, 0);
		while (!cls->coreInitialized) {wait(10);}
		send_ipi(apic_id, IPI_ENABLE_SCHEDULING_VECTOR, 0, 1, 0);
		kSMPInitDone = true;
    }
}

void ap_enable_schedulers() {
    for (int i = 0; i < kMPCoreCount; i++) {
        uint32_t apic_id = kCPUInfo[i].apicID;
        if (apic_id == BOOTSTRAP_PROCESSOR_ID) continue; // Skip BSP
        
        printd(DEBUG_SMP, "MP: Enabling scheduling on AP %u\n", apic_id);
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
    while (kTicksSinceStart < end) 
	{
		__asm__("nop\n");
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
    int displayedSpeed = cls->apicTicksPerSecond/1000/1000;
    printd(DEBUG_SMP, "Local APIC timer *adjusted* frequency is %u MHz (average %u ticks per second)\n", displayedSpeed,cls->apicTicksPerSecond);
}

// Interrupt handler for initializing an AP
void ap_initialization_handler() {
    
    // Get the APIC ID to identify the processor
    uint32_t apic_id = read_apic_id();  // Function to read the APIC ID register
    printd(DEBUG_SMP,"AP: initialization handler\n",apic_id);

    // NOTE: Stack was already initialized in ap_wakeup_entry

	//STAR MSR
	//Format: 63..48 | 47..32 | 31..16 | 15..0
	//--------|--------|--------|-------
	//Reserved| Kernel CS Segment Selector | User CS Segment Selector | Reserved (zeros)
	uint64_t starValue = ((uint64_t)GDT_KERNEL_CODE_ENTRY << 3) << 32 | ((GDT_USER_CODE_ENTRY << 3) | 3) << 16;

    wrmsr64(STAR_MSR,starValue);                      //Set sysenter CS(88) and SS(33), and return CS(93) & SS(3b)

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

	mp_determine_local_APIC_timer_speed();
	
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
    write_apic_register(kMPApicBase + APIC_TIMER_INIT_COUNT, cls->apicTimerCount * 3);  //Trigger X times per second based on config setting
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
    uint32_t val = read_apic_register(kMPApicBase + APIC_LVT_TIMER); // Use the read function
    //ConfigureAPITimer disables the timer, so enable it now
    val |= (1U << APIC_TIMER_PERIODIC_MODE_BIT);  // Ensure periodic mode is set
    val = ENABLE_TIMER(val);
    write_apic_register(kMPApicBase + APIC_LVT_TIMER, val);
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

void mpSendInvTLB()
{
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