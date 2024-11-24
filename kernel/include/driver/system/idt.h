#ifndef IDT_H
#define IDT_H

#include <stdint.h>

#include <stdint.h>

// IDT Entry for 64-bit mode
struct IDTEntry {
    uint16_t offset_low;    // Lower 16 bits of handler address
    uint16_t selector;      // Kernel code segment selector
    uint8_t  ist;           // Interrupt Stack Table (IST) offset
    uint8_t  type_attr;     // Type and attributes
    uint16_t offset_mid;    // Next 16 bits of handler address
    uint32_t offset_high;   // Upper 32 bits of handler address
    uint32_t zero;          // Reserved, must be 0
} __attribute__((packed));

// IDT Pointer
struct IDTPointer {
    uint16_t limit;         // Size of IDT (in bytes - 1)
    uint64_t base;          // Address of the IDT
} __attribute__((packed));

void initialize_idt();

#endif
