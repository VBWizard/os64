#include "driver/system/idt.h"
#include "smp_core.h"

extern void vector123();
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
extern void divide_by_zero_handler();
extern void invalid_opcode_handler();
extern void double_fault_handler();
extern void general_protection_fault_handler();
extern void page_fault_handler();
extern void machine_check_handler();

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

    // Set IRQ handlers
    set_idt_entry(0x20, (uint64_t)&handler_irq0_asm, 0x28, 0x8E); // IRQ0 (PIT)
    set_idt_entry(0x21, (uint64_t)&handler_irq1_asm, 0x28, 0x8E); // IRQ1 (Keyboard)

	// SET MP handlers
	set_idt_entry(IPI_INVALIDATE_TLB_VECTOR, (uint64_t)&vector123, 0x28, 0x8E);		// Invalidate TLB IPI
	set_idt_entry(IPI_DISABLE_SCHEDULING_VECTOR, (uint64_t)&vector124, 0x28, 0x8E);		// AP Disable IPI
	set_idt_entry(IPI_ENABLE_SCHEDULING_VECTOR, (uint64_t)&vector125, 0x28, 0x8E);		// AP Enable IPI
	set_idt_entry(IPI_TIMER_SCHEDULE_VECTOR, (uint64_t)&_schedule_ap, 0x28, 0x8E);		// AP Scheduler (timer ISR)
	set_idt_entry(IPI_AP_INITIALIZATION_VECTOR, (uint64_t)&vector127, 0x28, 0x8E);		// AP Initialization IPI
	set_idt_entry(IPI_MANUAL_SCHEDULE_VECTOR, (uint64_t)&_schedule_ap, 0x28, 0x8E);		// Scheduling IPI (calls same method as the vector158 AP Scheduler)

    // Load IDT
    asm volatile ("lidt %0" : : "m" (kIDTPtr));
}
