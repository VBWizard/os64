#ifndef GDT_H
#define GDT_H

#include <stdint.h>
#include <stdbool.h>

#define GDT_ENTRIES 128
#define GDT_KERNEL_CODE_ENTRY 5
#define GDT_KERNEL_DATA_ENTRY 6
#define GDT_USER_CODE_ENTRY   7
#define GDT_USER_DATA_ENTRY   8
#define GDT_FIRST_TSS_ENTRY   20
#define TSS_SELECTOR (GDT_FIRST_TSS_ENTRY << 3)
// Helper define for TSS
#define GDT_ACCESS_HELPER_TSS (GDT_ACCESS_PRESENT | GDT_ACCESS_EXECUTABLE) // TSS descriptor
// Helper for 
// Descriptor Types
#define GDT_ACCESS_KERNEL_CODE  0x9A  // Kernel Code: Executable, Readable, Present
#define GDT_ACCESS_KERNEL_DATA  0x92  // Kernel Data: Writable, Present
#define GDT_ACCESS_USER_CODE    0xFA  // User Code: Executable, Readable, Present
#define GDT_ACCESS_USER_DATA    0xF2  // User Data: Writable, Present
#define GDT_ACCESS_TSS          0x89  // Available TSS: Present

// Access Byte Bit Masks
#define GDT_ACCESS_ACCESSED     0x01  // Segment has been accessed
#define GDT_ACCESS_READWRITE    0x02  // Writable (data) or Readable (code)
#define GDT_ACCESS_DIRECTION    0x04  // Direction (data) or Conforming (code)
#define GDT_ACCESS_EXECUTABLE   0x08  // Executable (1 = Code segment)
#define GDT_ACCESS_DESCRIPTOR   0x10  // Descriptor type (1 = Code/Data, 0 = System)
#define GDT_ACCESS_RING0        0x00  // Privilege level 0 (Kernel)
#define GDT_ACCESS_RING3        0x60  // Privilege level 3 (User)
#define GDT_ACCESS_PRESENT      0x80  // Segment is present in memory
// Access Byte Helpers
// kernel descriptors
#define GDT_ACCESS_KERNEL_CODE_HELPER (GDT_ACCESS_PRESENT | GDT_ACCESS_EXECUTABLE | GDT_ACCESS_DESCRIPTOR | GDT_ACCESS_READWRITE | GDT_ACCESS_RING0)
#define GDT_ACCESS_KERNEL_DATA_HELPER (GDT_ACCESS_PRESENT | GDT_ACCESS_DESCRIPTOR | GDT_ACCESS_READWRITE | GDT_ACCESS_RING0)
// user descriptors
#define GDT_ACCESS_USER_CODE_HELPER (GDT_ACCESS_PRESENT | GDT_ACCESS_EXECUTABLE | GDT_ACCESS_DESCRIPTOR | GDT_ACCESS_READWRITE | GDT_ACCESS_RING3)
#define GDT_ACCESS_USER_DATA_HELPER (GDT_ACCESS_PRESENT | GDT_ACCESS_DESCRIPTOR | GDT_ACCESS_READWRITE | GDT_ACCESS_RING3)

// Flags Byte Bit Masks
// Flags - Granularity
#define GDT_FLAG_GRANULARITY_1B  0x00  // Byte granularity
#define GDT_FLAG_GRANULARITY_4K  0x80  // 4 KiB granularity
// Flags - size
#define GDT_FLAG_SIZE_16BIT  0x00  // 16-bit segment
#define GDT_FLAG_SIZE_32BIT  0x40  // 32-bit segment
// Flags - long mode
#define GDT_FLAG_LONG_MODE   0x20  // 64-bit code segment
// Flags - available
#define GDT_FLAG_AVAILABLE   0x10  // Available for OS use
// Flags Helpers
// Flat 64 code segment
#define GDT_FLAGS_64BIT_CODE (GDT_FLAG_GRANULARITY_4K | GDT_FLAG_LONG_MODE)
// Flat 32 code/data segment
#define GDT_FLAGS_32BIT (GDT_FLAG_GRANULARITY_4K | GDT_FLAG_SIZE_32BIT)
// 16-bit legacy segment
#define GDT_FLAGS_16BIT (GDT_FLAG_GRANULARITY_1B | GDT_FLAG_SIZE_16BIT)
#define GDT_FLAGS_64BIT_DATA (GDT_FLAG_GRANULARITY_4K)

// S flag settings
#define GDT_S_CODE_DATA_SEGMENT  1  // Code/Data Segment (S = 1)
#define GDT_S_SYSTEM_SEGMENT     0 // System Segment (S = 0)

typedef struct {
    uint16_t limit;  // Size of the GDT - 1
    uint64_t base;   // Base address of the GDT
} __attribute__((packed)) gdt_pointer_t;

typedef struct {
    uint16_t limit_low;       // Lower 16 bits of segment limit
    uint16_t base_low;        // Lower 16 bits of base address
    uint8_t base_middle;      // Next 8 bits of base address
    uint8_t access;           // Access flags
    uint8_t flags_and_limit;  // Upper 4 bits of limit and flags
    uint8_t base_high;        // Next 8 bits of base address
} __attribute__((packed)) gdt_entry_t;

// Second 8 bytes of a system descriptor (TSS, LDT, etc.)
typedef struct {
    uint16_t base_low;        // Lower 16 bits of upper 32 bits of base
    uint8_t base_middle;      // Next 8 bits of upper 32 bits of base
    uint8_t base_high;        // Highest 8 bits of base
    uint32_t reserved;        // Reserved, must be zero
} __attribute__((packed)) gdt_entry_additional_t;


/// @brief Set a non-system GDT entry
void set_gdt_entry( gdt_entry_t* gdt_table, int entryNo, uint64_t base, uint32_t limit, uint8_t access, uint8_t flags, uint8_t setSFlag);
/// @brief Set a system GDT entry (TSS/LDT/Other)
void init_GDT();

extern gdt_entry_t *kGDT;
extern gdt_pointer_t kGDTr;
#endif
