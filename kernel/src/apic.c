#include <stdbool.h>
#include "apic.h"
#include "acpi.h"
#include "cpu.h"
#include "smp.h"
#include "msr.h"
#include "time.h"
#include "x86_64.h"
#include "io.h"
#include "serial_logging.h"

volatile bool kIRQ0UsesLapic = false;
volatile bool kIRQ1UsesLapic = false;
volatile bool kIRQ12UsesLapic = false;

static void set_imcr_to_apic(void)
{
    outb(0x22, 0x70);
    uint8_t value = inb(0x23);
    outb(0x23, value | 0x01);
}

static uint8_t get_ioapic_redirection_index_for_irq(uint8_t irq)
{
    // First choice: the ACPI MADT's Interrupt Source Overrides — the modern,
    // guaranteed source for "this ISA IRQ is wired to a different IOAPIC
    // input". The PIT is the canonical case: ISA IRQ 0 -> GSI 2 on
    // VirtualBox and most real hardware. Relying on the MP tables alone
    // routed IRQ0 to a masked pin on VBox and froze the system heartbeat
    // (ticks stopped the instant IRQ0 moved from PIC to IOAPIC).
    if (irq < 16 && kISAIrqToGSI[irq] >= 0) {
        return (uint8_t)kISAIrqToGSI[irq];
    }

    // Fallback: legacy Intel MP tables (what QEMU/SeaBIOS happened to make
    // work). NOTE the srcbus==1 assumption — MP bus numbering is
    // BIOS-enumerated, so ISA being bus 1 is a SeaBIOS convention, not a
    // rule; that's why the MADT overrides above are checked first.
    if (!kMPConfigTable || !kMPConfigTableCount) {
        return irq;
    }

    for (uint32_t i = 0; i < kMPConfigTableCount; ++i) {
        if (kMPConfigTable[i].recType != IOINTASS) {
            continue;
        }

        struct mpc_intsrc entry = kMPConfigTable[i].irqSrc;
        // Bus 1 corresponds to ISA in the MP tables; that's where PIT lives.
        if (entry.srcbus == 1 && entry.srcbusirq == irq) {
            return entry.dstirq;
        }
    }

    return irq;
}

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

// Program the IOAPIC redirection entry that firmware says this ISA IRQ is
// wired to (MADT override first, MP tables second), targeting the given
// LAPIC. Returns false when there is no IOAPIC to program.
static bool ioapic_route_irq(uint8_t isa_irq, uint8_t vector, uint8_t dest_apic_id) {
    if (!kIOAPICAddress) {
        return false;
    }

    uint8_t redirection_index = get_ioapic_redirection_index_for_irq(isa_irq);
    uint32_t low_reg = 0x10 + ((uint32_t)redirection_index * 2);
    uint32_t high_reg = low_reg + 1;

    uint32_t entry_low = vector; // delivery mode fixed; edge/high unless the override says otherwise
    uint32_t entry_high = ((uint32_t)dest_apic_id) << 24; // physical destination

    // Honor the override's MPS INTI flags: polarity (bits 0-1, 11 = active
    // low -> redirection bit 13) and trigger mode (bits 2-3, 11 = level ->
    // redirection bit 15). 00 means "conforms to bus" = edge/high for ISA,
    // which is the default already encoded in entry_low.
    if (kISAIrqToGSI[isa_irq] >= 0) {
        uint16_t iso_flags = kISAIrqOverrideFlags[isa_irq];
        if ((iso_flags & 0x3) == 0x3)
            entry_low |= (1u << 13);   // active low
        if (((iso_flags >> 2) & 0x3) == 0x3)
            entry_low |= (1u << 15);   // level triggered
    }

    ioapic_write(high_reg, entry_high);
    ioapic_write(low_reg, entry_low);

    printd(DEBUG_SMP, "IOAPIC: IRQ%u mapped to redirection %u, vector 0x%02x, dest APIC %u\n",
           isa_irq, redirection_index, vector, dest_apic_id);

    return true;
}

// Move an ISA IRQ from the legacy PIC to the IOAPIC: program the redirection
// entry (targeting dest_apic_id), mask the line on the PIC so it can't
// double-deliver, and flip the handler's EOI-path flag (each IRQ's asm
// handler tests its own k*UsesLapic to choose LAPIC vs PIC EOI).
//
// Why this exists for MORE than IRQ0: set_imcr_to_apic() disconnects the
// PIC's INTR wire from the CPU. After that, PIC-delivered interrupts only
// still arrive if firmware happened to leave LINT0 in virtual-wire mode —
// true on QEMU, NOT guaranteed on VBox or real hardware. Any IRQ we actually
// depend on must therefore ride the IOAPIC.
//
// The destination matters more than it looks: the GUI routes the input IRQs
// (1 and 12) at the COMPOSITOR's core, so a keyboard/mouse interrupt ends
// the compositor's hlt-wait directly — input latency without busy-waiting.
void ioapic_adopt_isa_irq(uint8_t isa_irq, uint8_t vector, uint8_t dest_apic_id, volatile bool *uses_lapic_flag) {
    if (!ioapic_route_irq(isa_irq, vector, dest_apic_id)) {
        return;
    }

    if (isa_irq < 8) {
        outb(PIC1_DATA, inb(PIC1_DATA) | (uint8_t)(1u << isa_irq));
    } else {
        outb(PIC2_DATA, inb(PIC2_DATA) | (uint8_t)(1u << (isa_irq - 8)));
    }

    *uses_lapic_flag = true;
}

void remap_irq0_to_apic(uint32_t vector) {
    if (!kIOAPICAddress) {
        return;
    }

    // Route PIT IRQ0 through the IOAPIC (to the BSP — it owns timekeeping),
    // then switch the platform to APIC mode. The IMCR write is deliberately
    // NOT in ioapic_adopt_isa_irq — it is a one-time platform-wide switch,
    // not a per-IRQ action.
    ioapic_adopt_isa_irq(0, (uint8_t)(vector & 0xFF), kCPUInfo[0].apicID, &kIRQ0UsesLapic);

    set_imcr_to_apic();
}
