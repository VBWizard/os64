#include "gdt.h"
#include "kmalloc.h"
#include "tss.h"

gdt_entry_t *kGDT;
gdt_pointer_t kGDTr;


void set_gdt_entry_additional_for_system_descriptors(gdt_entry_additional_t* additional_entry, uint64_t base) 
{
    additional_entry->base_low = (base >> 32) & 0xFFFF;   // Low 16 bits of high 32 bits
    additional_entry->base_middle = (base >> 48) & 0xFF; // Next 8 bits
    additional_entry->base_high = (base >> 56) & 0xFF;   // Highest 8 bits
    additional_entry->reserved = 0;                      // Reserved, must be 0
}

void set_gdt_entry(gdt_entry_t* gdt_table, int entryNo, uint64_t base, uint32_t limit, uint8_t access, uint8_t flags, uint8_t isSystem) 
{
    // Set the first 8-byte entry
    gdt_table[entryNo].base_low = base & 0xFFFF;			//32 bits
    gdt_table[entryNo].base_middle = (base >> 16) & 0xFF;	//8 bits
    gdt_table[entryNo].base_high = (base >> 24) & 0xFF;		//8 bits - 48 bits total
    gdt_table[entryNo].limit_low = limit & 0xFFFF;
    gdt_table[entryNo].flags_and_limit = (flags & 0xF0) | ((limit >> 16) & 0x0F);
    gdt_table[entryNo].access = access;

    // Handle system descriptor setup
    if (isSystem)
        set_gdt_entry_additional_for_system_descriptors((gdt_entry_additional_t*)&gdt_table[entryNo + 1], base);
	else
        gdt_table[entryNo].access |= 0x10;
}


extern void load_gdt_and_jump(gdt_pointer_t *gdtr);

void init_GDT()
{
	kGDT = kmalloc(GDT_ENTRIES * sizeof(gdt_entry_t));

	set_gdt_entry(kGDT, GDT_KERNEL_CODE_ENTRY, 0, 0xFFFFF, GDT_ACCESS_KERNEL_CODE_HELPER, GDT_FLAGS_64BIT_CODE, GDT_S_CODE_DATA_SEGMENT);
	set_gdt_entry(kGDT, GDT_KERNEL_DATA_ENTRY, 0, 0xFFFFF, GDT_ACCESS_KERNEL_DATA_HELPER, GDT_FLAGS_64BIT_DATA, GDT_S_CODE_DATA_SEGMENT);

	kGDTr.base = (uintptr_t)kGDT;
	kGDTr.limit = sizeof(gdt_entry_t) * GDT_ENTRIES;

	load_gdt_and_jump(&kGDTr);

	init_tss();
}