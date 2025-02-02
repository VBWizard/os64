#include "driver/system/idt.h"

extern void vector155();
extern void vector156();
extern void vector157();
extern void vector158();
extern void vector159();
extern void vector161();
extern void _schedule_ap();
struct IDTEntry kIDT[256];
struct IDTPointer kIDTPtr;

extern void handler_irq0_asm();
extern void handler_irq1_asm();
extern void divide_by_zero_handler();
extern void general_protection_fault_handler();
extern void page_fault_handler();
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

    // Set exception handlers
    set_idt_entry(0x00, (uint64_t)&divide_by_zero_handler, 0x28, 0x8E); // Example
    set_idt_entry(0x0D, (uint64_t)&general_protection_fault_handler, 0x28, 0x8E);
    set_idt_entry(0x0E, (uint64_t)&page_fault_handler, 0x28, 0x8E);

    // Set IRQ handlers
    set_idt_entry(0x20, (uint64_t)&handler_irq0_asm, 0x28, 0x8E); // IRQ0 (PIT)
    set_idt_entry(0x21, (uint64_t)&handler_irq1_asm, 0x28, 0x8E); // IRQ1 (Keyboard)

	// SET MP handlers
	set_idt_entry(0x7b, (uint64_t)&vector155, 0x28, 0x8E);		// Invalidate TLB IPI
	set_idt_entry(0x7c, (uint64_t)&vector156, 0x28, 0x8E);		// AP Disable IPI
	set_idt_entry(0x7d, (uint64_t)&vector157, 0x28, 0x8E);		// AP Enable IPI
	set_idt_entry(0x7e, (uint64_t)&_schedule_ap, 0x28, 0x8E);		// AP Scheduler (timer ISR)
	set_idt_entry(0x7f, (uint64_t)&vector159, 0x28, 0x8E);		// AP Initialization IPI
	set_idt_entry(0x81, (uint64_t)&_schedule_ap, 0x28, 0x8E);		// Scheduling IPI (calls same method as the vector158 AP Scheduler)

    // Load IDT
    asm volatile ("lidt %0" : : "m" (kIDTPtr));
}
