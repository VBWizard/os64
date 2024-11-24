#include "tss.h"
#include "gdt.h"

tss_t kInitialTSS;

void init_tss() {
    // Define the TSS
    kInitialTSS.rsp0 = (uint64_t)0xffffffff90000000;  // Set kernel stack pointer
    kInitialTSS.iomap_base = sizeof(tss_t);       // No I/O permission map

    // Add TSS to the GDT (entries 8 and 9)
    uint64_t tss_base = (uint64_t)&kInitialTSS;
    uint32_t tss_limit = sizeof(tss_t) - 1;

 	set_gdt_entry(kGDT, 8, tss_base, tss_limit, 0x89, 0x00, 1);  // TSS descriptor

	gdt_pointer_t gdtr;
	asm volatile("sgdt %0" : "=m"(gdtr));

    // Load the TSS descriptor into TR
    asm volatile (
        "mov ax, 0x40\n\t"  // TSS selector (0x40 = index 8 in GDT)
        "ltr ax\n\t"        // Load TR
        :
        :
        : "memory"
    );
}
