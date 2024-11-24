#ifndef TSS_H
#define TSS_H

#include <stdint.h>

typedef struct  {
    uint16_t limit_low;          // Bits 0-15 of the TSS size
    uint16_t base_low;           // Bits 0-15 of the TSS base address
    uint8_t base_mid;            // Bits 16-23 of the TSS base address
    uint8_t type : 4;            // Type (9 for available 64-bit TSS)
    uint8_t zero : 1;            // Always 0
    uint8_t dpl : 2;             // Descriptor Privilege Level
    uint8_t present : 1;         // Present bit
    uint8_t limit_high : 4;      // Bits 16-19 of the TSS size
    uint8_t available : 1;       // Available for system software
    uint8_t zero1 : 2;           // Always 0
    uint8_t granularity : 1;     // Granularity (0 for byte granularity)
    uint8_t base_high;           // Bits 24-31 of the TSS base address
    uint32_t base_upper;         // Bits 32-63 of the TSS base address
    uint32_t reserved;           // Reserved, must be 0
} __attribute__((packed)) tss_descriptor_t;

typedef struct {
    uint32_t reserved1;    // Reserved, must be 0
    uint64_t rsp0;         // Stack pointer for Ring 0
    uint64_t rsp1;         // Stack pointer for Ring 1
    uint64_t rsp2;         // Stack pointer for Ring 2
    uint64_t reserved2;    // Reserved, must be 0
    uint64_t ist1;         // Interrupt Stack Table 1
    uint64_t ist2;         // Interrupt Stack Table 2
    uint64_t ist3;         // Interrupt Stack Table 3
    uint64_t ist4;         // Interrupt Stack Table 4
    uint64_t ist5;         // Interrupt Stack Table 5
    uint64_t ist6;         // Interrupt Stack Table 6
    uint64_t ist7;         // Interrupt Stack Table 7
    uint64_t reserved3;    // Reserved, must be 0
    uint16_t reserved4;    // Reserved, must be 0
    uint16_t iomap_base;   // I/O Map Base Address
} __attribute__((packed)) tss_t;

void init_tss();

#endif
