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
	keyboard_init();
    asm("sti\n");
    printd(DEBUG_BOOT, "Interrupts enabled\n");

	struct tm date_time_buff = getRTCDate();
    kSystemStartTime = mktime(&date_time_buff);

    // The kernel's standard-time offset, hours east of UTC. If the cmdline
    // carried a TZ= string (classic format: EST5EDT), derive it from there —
    // skip the leading zone name, read the offset, and remember TZ counts
    // hours WEST of UTC (V7's little prank: Eastern is 5, Tokyo is -9), so
    // east-positive kTimeZone is its negation. ONLY the standard offset is
    // taken here: DST is legislation, legislation lives in libos64's
    // calendar, and the kernel's own few displays (boot prints, the FAT
    // timestamp glue) stay honest-if-boring standard time year-round.
    kTimeZone = -5;   // default: Eastern standard, as it always was
    if (kTZString[0])
    {
        const char *p = kTZString;
        while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))
            p++;
        int sign = 1;
        if (*p == '+')
            p++;
        else if (*p == '-')
        {
            sign = -1;
            p++;
        }
        if (*p >= '0' && *p <= '9')
        {
            int hours = 0;
            while (*p >= '0' && *p <= '9')
                hours = hours * 10 + (*p++ - '0');
            kTimeZone = -(sign * hours);   // west-positive in, east-positive out
        }
    }

    kSystemCurrentTime = kSystemStartTime;
}
