#include "limine.h"
#include "allocator.h"
#include "paging.h"
#include "memmap.h"
#include "driver/system/pit.h"
#include "driver/system/pic.h"
#include "driver/system/idt.h"
#include "driver/system/keyboard.h"
#include "serial_logging.h"
#include "rtc.h"

#define PIC_REMAP_OFFSET 32

extern void init_PIT();

extern volatile uint64_t kSystemStartTime, kUptime;
extern volatile uint64_t kSystemCurrentTime;
extern volatile int kTimeZone;

void hardware_init()
{
	// Tell the compiler we want intel syntax
	__asm__(".intel_syntax noprefix");
	// Disable interrupts
	__asm__("cli\n");
	// Put the CR3 value in KpageDirectoryBaseAddress
  	__asm__("mov rax, cr3\n"
    		"mov %0, rax\n" : "=r"(kKernelPML4));


	//I decided to go with init_PIT (assembly) instead of initialize_pit_timer (C) b/c when using init_PIT the drift is almost impreceptable
	__asm__ ("mov rbx, %[ticks]\n" // Move TICKS_PER_SECOND into RBX
    "call init_PIT\n"     // Call the init_PIT function
    :
    : [ticks] "r" ((uint64_t)TICKS_PER_SECOND) // Input operand
    : "rbx"                          // Clobbered register
);
	pic_remap(0 + PIC_REMAP_OFFSET, 8 + PIC_REMAP_OFFSET);
	initialize_idt();
	keyboard_init();
    asm("sti\n");
    printd(DEBUG_BOOT, "Interrupts enabled\n");

	struct tm date_time_buff = getRTCDate();
    kSystemStartTime = mktime(&date_time_buff);
    kTimeZone = -5;
    kSystemCurrentTime = kSystemStartTime;
}
