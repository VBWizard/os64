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
#include "fpu.h"

#define PIC_REMAP_OFFSET 32

extern void init_PIT();

extern volatile uint64_t kSystemStartTime, kUptime;
extern volatile uint64_t kSystemCurrentTime;
extern char kTZString[];

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
	fpu_init_this_cpu();
	keyboard_init();
    asm("sti\n");
    printd(DEBUG_BOOT, "Interrupts enabled\n");

	struct tm date_time_buff = getRTCDate();
    kSystemStartTime = mktime(&date_time_buff);

    // The kernel's standard-time offset, from the cmdline's TZ= if there is
    // one and GMT otherwise (time_set_zone). This is the zone the kernel's
    // own few displays use until a filesystem exists; bootenv.conf can move
    // it once one does (bootenv.c), and the environment every program reads
    // is settled there too. The machine speaks UTC unless told otherwise —
    // it was Eastern (-5) from the first OS until 2026-08-04 — because the
    // computer has a location, but the timezone belongs to whoever is
    // looking at it.
    time_set_zone(kTZString);

    kSystemCurrentTime = kSystemStartTime;
}
