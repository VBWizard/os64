#include "limine.h"
#include "allocator.h"
#include "paging.h"
#include "memmap.h"
#include "driver/system/pit.h"
#include "driver/system/pic.h"
#include "driver/system/idt.h"
#include "serial_logging.h"
#include "io.h"

extern void init_PIT();

void hardware_init()
{
	// Tell the compiler we want intel syntax
	__asm__(".intel_syntax noprefix");
	// Disable interrupts
	__asm__("cli\n");
	// Put the CR3 value in KpageDirectoryBaseAddress
  	__asm__("mov rax, cr3\n"
    		"mov %0, rax\n" : "=r"(kKernelPML4));
	
	//__asm__("mov rbx, 10\ncall init_PIT\n");
	initialize_pit_timer(TICKS_PER_SECOND);
	pic_remap(0x20,0x28);
	initialize_idt();
	//Enable interrupts
	asm ("sti\n");
	printd(DEBUG_BOOT, "Interrupts enabled\n");
}