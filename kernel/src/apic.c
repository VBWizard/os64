#include <stdbool.h>
#include "apic.h"
#include "cpu.h"
#include "smp.h"
#include "msr.h"
#include "time.h"
#include "x86_64.h"

bool apicCheckFor() {
   uint32_t eax=0, edx=0, notused=0;
   __get_cpuid(1, &eax, &notused, &notused, &edx);
   return edx & CPUID_FLAG_APIC;
}

uint8_t acpiGetAPICVersion()
{
    return apicReadRegister(APIC_REGISTER_VERSION);
}

uint8_t apciGetAPICID()
{
    return apicReadRegister(APIC_REGISTER_APIC_ID_OFFSET);
}

uint64_t apicGetAPICBase(void)
{
   uint64_t rax = rdmsr64(IA32_APIC_BASE_MSR);

   return (rax & 0xfffffffffffff000);
}

uint32_t apicReadRegister(uint32_t reg) 
{
    return *((volatile uint32_t *) (kCPUInfo[0].registerBase + reg));
}

void apicWriteRegister(uint64_t reg, uint32_t value) {
    *((volatile uint32_t *) (kCPUInfo[0].registerBase + reg)) = value;
}

/* Set the physical address for local APIC registers */
void apicSetAPICBase(uintptr_t apic) {
    uint64_t msr_value;

    // Combine the base address with the enable flag
    msr_value = (apic & 0xFFFFFFFFFFFFF100) | IA32_APIC_BASE_MSR_ENABLE;

    // Write the value to the IA32_APIC_BASE MSR
    wrmsr64(IA32_APIC_BASE_MSR, msr_value);

    // Update the CPU's APIC base register tracking
    kCPUInfo[0].registerBase = apic;
}

/**
 * Get the physical address of the APIC registers page
 * make sure you map it to virtual memory ;)
 */
uintptr_t cpu_get_apic_base() {
   uint64_t rax = rdmsr64(IA32_APIC_BASE_MSR);
 
#ifdef __PHYSICAL_MEMORY_EXTENSION__
   return (eax & 0xfffff000) | ((edx & 0x0f) << 32);
#else
   return (rax & 0xfffff000);
#endif
}

bool apicIsEnabled() {
   uint64_t value;
   value = rdmsr64(IA32_APIC_BASE_MSR);
   return (value & (1 << 11)) != 0;
} 

void apicEnable() {
   uint64_t value;
   value = rdmsr64(IA32_APIC_BASE_MSR);
   value |= IA32_APIC_BASE_MSR_ENABLE;
   wrmsr64(IA32_APIC_BASE_MSR, value);
} 

void apicDisable() {
    uint64_t value = rdmsr64(IA32_APIC_BASE_MSR); // Read the current value of the MSR
    value &= ~IA32_APIC_BASE_MSR_ENABLE;         // Clear the APIC enable bit (bit 11)
    wrmsr64(IA32_APIC_BASE_MSR, value);          // Write the modified value back to the MSR
}

uint32_t apicGetHZ() {

    int timerTimeout=10;
    // Prepare the PIT to sleep for 10ms (10000µs)
    apicEnable();
    apicWriteRegister(APIC_REGISTER_SPURIOUS, 39+APIC_SW_ENABLE);
    // Set APIC init counter to -1
    apicWriteRegister(APIC_REGISTER_LVT_TIMER, (32 | APIC_TIMER_MODE_ONESHOT) & ~0x10000);
    // Tell APIC timer to use divider 16
    apicWriteRegister(APIC_REGISTER_TIMER_DIV, 0x11);
    apicWriteRegister(APIC_REGISTER_TIMER_INITIAL, 0xFFFFFFFF);

    // Perform PIT-supported sleep
    kwait(timerTimeout);

    apicWriteRegister(APIC_REGISTER_LVT_TIMER, APIC_TIMER_INT_DISABLE);
    // Now we know how often the APIC timer has ticked in 10ms
    uint64_t ticksPer10ms = 0xFFFFFFFF - apicReadRegister(APIC_REGISTER_TIMER_CURRENT);
    ticksPer10ms=ticksPer10ms/(timerTimeout/10);
    
    // Start timer as periodic on IRQ 0, divider 16, with the number of ticks we counted
//    apicWriteRegister(APIC_REGISTER_LVT_TIMER, 32  | APIC_TIMER_MODE_PERIODIC ); //clears the INT DISABLE pin
    apicWriteRegister(APIC_REGISTER_TIMER_DIV, 0x3);
    apicWriteRegister(APIC_REGISTER_TIMER_INITIAL, ticksPer10ms);
    return ticksPer10ms;
}

void ioapic_write(uint32_t reg, uint32_t value) {
    volatile uint32_t *ioapic_base = (uint32_t *)kIOAPICAddress;
    ioapic_base[IOAPIC_REGSEL_OFFSET / 4] = reg;
    ioapic_base[IOAPIC_WIN_OFFSET / 4] = value;
}

void remap_irq0_to_apic(uint32_t vector) {
    //Remap IR0 from the PIC to the calling core's IO APIC
	// IOAPIC Redirection Table Entry for IRQ0 (Index 0x10 and 0x11)
    uint32_t irq0_low = vector;  // Vector number and flags (e.g., fixed delivery mode)
    uint32_t irq0_high = 0x00000000;  // Destination APIC ID

    // Write high 32 bits first, then low 32 bits
    ioapic_write(0x11, irq0_high);
    ioapic_write(0x10, irq0_low);
}
