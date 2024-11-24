#include "driver/system/idt.h"


struct IDTEntry idt[256];
struct IDTPointer idt_ptr;

extern void handler_irq0_asm();
extern void handler_irq1_asm();
extern void divide_by_zero_handler();
extern void general_protection_fault_handler();
extern void page_fault_handler();
// Set an IDT entry
void set_idt_entry(int vector, uint64_t handler, uint16_t selector, uint8_t type_attr) {
    idt[vector].offset_low = handler & 0xFFFF;
    idt[vector].selector = selector;
    idt[vector].ist = 0; // No IST by default
    idt[vector].type_attr = type_attr;
    idt[vector].offset_mid = (handler >> 16) & 0xFFFF;
    idt[vector].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[vector].zero = 0;
}

// Initialize IDT
void initialize_idt() {
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base = (uint64_t)&idt;

    // Set exception handlers
    set_idt_entry(0x00, (uint64_t)&divide_by_zero_handler, 0x28, 0x8E); // Example
    set_idt_entry(0x0D, (uint64_t)&general_protection_fault_handler, 0x28, 0x8E);
    set_idt_entry(0x0E, (uint64_t)&page_fault_handler, 0x28, 0x8E);

    // Set IRQ handlers
    set_idt_entry(0x20, (uint64_t)&handler_irq0_asm, 0x28, 0x8E); // IRQ0 (PIT)
    set_idt_entry(0x21, (uint64_t)&handler_irq1_asm, 0x28, 0x8E); // IRQ1 (Keyboard)

    // Load IDT
    asm volatile ("lidt %0" : : "m" (idt_ptr));
}
