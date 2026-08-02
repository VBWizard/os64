#include "driver/system/idt.h"
#include "smp_core.h"

extern void vector123();
extern void vector130();
extern void vector124();
extern void vector125();
extern void vector138();
extern void vector127();
extern void vector141();
extern void _schedule_ap();
struct IDTEntry kIDT[256];
struct IDTPointer kIDTPtr;

extern void handler_irq0_asm();
extern void handler_irq1_asm();
extern void handler_irq12_asm();
extern void divide_by_zero_handler();
extern void invalid_opcode_handler();
extern void double_fault_handler();
extern void general_protection_fault_handler();
extern void page_fault_handler();
extern void machine_check_handler();
// The vectors that had no gate until 2026-08-01 — see handler_errors.S for
// why an empty gate is worse than a handler that only names the fault.
extern void debug_exception_handler();
extern void nmi_handler();
extern void breakpoint_handler();
extern void overflow_handler();
extern void bound_range_handler();
extern void device_not_available_handler();
extern void invalid_tss_handler();
extern void segment_not_present_handler();
extern void stack_segment_handler();
extern void x87_fpu_handler();
extern void alignment_check_handler();
extern void simd_fpu_handler();

// Set an IDT entry
void set_idt_entry(int vector, uint64_t handler, uint16_t selector, uint8_t type_attr) {
    kIDT[vector].offset_low = handler & 0xFFFF;
    kIDT[vector].selector = selector;
    kIDT[vector].ist = 0; // No IST by default
    kIDT[vector].type_attr = type_attr;
    kIDT[vector].offset_mid = (handler >> 16) & 0xFFFF;
    kIDT[vector].offset_high = (handler >> 32) & 0xFFFFFFFF;
    kIDT[vector].zero = 0;
}

// Initialize IDT
void initialize_idt() {
    kIDTPtr.limit = sizeof(kIDT) - 1;
    kIDTPtr.base = (uint64_t)&kIDT;

	// IDT Entries for all major exceptions, all using 0x8E (Interrupt Gate)
	set_idt_entry(0x00, (uint64_t)&divide_by_zero_handler, 0x28, 0x8E); // #DE
	set_idt_entry(0x06, (uint64_t)&invalid_opcode_handler, 0x28, 0x8E); // #UD
	set_idt_entry(0x08, (uint64_t)&double_fault_handler, 0x28, 0x8E);
	set_idt_entry(0x0D, (uint64_t)&general_protection_fault_handler, 0x28, 0x8E); // #GP
	set_idt_entry(0x0E, (uint64_t)&page_fault_handler, 0x28, 0x8E); // #PF
	set_idt_entry(0x12, (uint64_t)&machine_check_handler, 0x28, 0x8E); // #MC
	// The rest of the CPU's exception range. Nothing here RECOVERS — they
	// name the fault and panic, which is strictly better than the #GP an
	// empty gate produces (that #GP names the IDT slot, not the problem,
	// and blames whatever code was running when it happened).
	set_idt_entry(0x01, (uint64_t)&debug_exception_handler, 0x28, 0x8E);      // #DB
	set_idt_entry(0x02, (uint64_t)&nmi_handler, 0x28, 0x8E);                  // NMI
	set_idt_entry(0x03, (uint64_t)&breakpoint_handler, 0x28, 0x8E);           // #BP
	set_idt_entry(0x04, (uint64_t)&overflow_handler, 0x28, 0x8E);             // #OF
	set_idt_entry(0x05, (uint64_t)&bound_range_handler, 0x28, 0x8E);          // #BR
	set_idt_entry(0x07, (uint64_t)&device_not_available_handler, 0x28, 0x8E); // #NM
	set_idt_entry(0x0A, (uint64_t)&invalid_tss_handler, 0x28, 0x8E);          // #TS
	set_idt_entry(0x0B, (uint64_t)&segment_not_present_handler, 0x28, 0x8E);  // #NP
	set_idt_entry(0x0C, (uint64_t)&stack_segment_handler, 0x28, 0x8E);        // #SS
	set_idt_entry(0x10, (uint64_t)&x87_fpu_handler, 0x28, 0x8E);              // #MF
	set_idt_entry(0x11, (uint64_t)&alignment_check_handler, 0x28, 0x8E);      // #AC
	set_idt_entry(0x13, (uint64_t)&simd_fpu_handler, 0x28, 0x8E);             // #XM

    // Set IRQ handlers
    set_idt_entry(0x20, (uint64_t)&handler_irq0_asm, 0x28, 0x8E); // IRQ0 (PIT)
    set_idt_entry(0x21, (uint64_t)&handler_irq1_asm, 0x28, 0x8E); // IRQ1 (Keyboard), legacy-PIC vector
    set_idt_entry(0x2C, (uint64_t)&handler_irq12_asm, 0x28, 0x8E); // IRQ12 (PS/2 mouse), legacy-PIC vector

    // IOAPIC-delivered input IRQs use these HIGHER vectors instead: the APs
    // run with LAPIC TPR = 0x30 (smp_core.c), which silently masks every
    // vector below 0x40 — the legacy 0x2x vectors never fire on an AP. The
    // GUI routes keyboard/mouse at the compositor's core, so their vectors
    // must clear that bar. Same handlers; EOI logic is vector-agnostic.
    set_idt_entry(0x41, (uint64_t)&handler_irq1_asm, 0x28, 0x8E); // IRQ1 via IOAPIC
    set_idt_entry(0x4C, (uint64_t)&handler_irq12_asm, 0x28, 0x8E); // IRQ12 via IOAPIC

	// SET MP handlers
	set_idt_entry(IPI_INVALIDATE_TLB_VECTOR, (uint64_t)&vector123, 0x28, 0x8E);		// Invalidate TLB IPI
	set_idt_entry(IPI_ACCT_SETTLE_VECTOR, (uint64_t)&vector130, 0x28, 0x8E);		// CPU-time settle-on-read IPI
	set_idt_entry(IPI_DISABLE_SCHEDULING_VECTOR, (uint64_t)&vector124, 0x28, 0x8E);		// AP Disable IPI
	set_idt_entry(IPI_ENABLE_SCHEDULING_VECTOR, (uint64_t)&vector125, 0x28, 0x8E);		// AP Enable IPI
	set_idt_entry(IPI_TIMER_SCHEDULE_VECTOR, (uint64_t)&_schedule_ap, 0x28, 0x8E);		// AP Scheduler (timer ISR)
	set_idt_entry(IPI_AP_INITIALIZATION_VECTOR, (uint64_t)&vector127, 0x28, 0x8E);		// AP Initialization IPI
	set_idt_entry(IPI_MANUAL_SCHEDULE_VECTOR, (uint64_t)&_schedule_ap, 0x28, 0x8E);		// Scheduling IPI (calls same method as the vector158 AP Scheduler)

    // Load IDT
    asm volatile ("lidt %0" : : "m" (kIDTPtr));
}
