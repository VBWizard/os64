#include "CONFIG.h"
#include "serial_logging.h"
#include "allocator.h"
#include "memory/paging.h"   // paging_pool_pages_used — the park loop's pool probe
#include "BasicRenderer.h"
#include "strings/sprintf.h"
#include "strftime.h"
#include "signals.h"
#include "task.h"
#include "io.h"

int usedCount=0;
extern volatile uint64_t kSystemCurrentTime;
extern volatile uint64_t kTicksSinceStart;
extern BasicRenderer kRenderer;
extern int kTimeZone;
extern volatile uint64_t kSystemStartTime;
extern task_t* kKernelTask;

void shutdown()
{
	uint64_t memInUse=0;
    uint64_t lastTime = 0;
    char heartbeat[2] = "*";
    printd(DEBUG_SHUTDOWN, "BOOT END: Status of memory status (%u entries):\n",kMemoryStatusCurrentPtr);
	for (uint64_t cnt=0;cnt<kMemoryStatusCurrentPtr;cnt++)
	{
		printd(DEBUG_SHUTDOWN, "\tMemory at 0x%016Lx for 0x%016Lx (%Lu) bytes is %s\n",kMemoryStatus[cnt].startAddress, kMemoryStatus[cnt].length, kMemoryStatus[cnt].length, kMemoryStatus[cnt].in_use?"in use":"not in use");
		if (kMemoryStatus[cnt].in_use)
		{
			usedCount++;
			memInUse+=kMemoryStatus[cnt].length;
		}
	}
	printd(DEBUG_SHUTDOWN, "Found %u memory in use at shutdown\n", memInUse);
	printd(DEBUG_SHUTDOWN, "Found %u memory status entries,  %u in use\n", kMemoryStatusCurrentPtr, usedCount);
	// This used to say "Idling for now until we have a userland to run!" — and
	// as of husk, that is no longer true. The kernel task doesn't idle here
	// waiting for a userland; it parks here BECAUSE the userland is running.
	// The scheduler keeps husk (and whatever husk spawns) on the CPUs while
	// this loop naps and repaints the clock. os64 is a system that stays up.
	printf("Kernel loaded! Userland is running — the kernel task parks here.\n");

	// Boot-complete pool receipt (probe, 2026-08-04, the 640-page exhaustion
	// hunt): how much of the paging pool did BOOT consume? Straight to the
	// serial wire — panic's door — so it's readable live even when a logd
	// claim owns the log. Splits the mystery in two: everything after this
	// line is runtime consumption, and the park loop below reports its slope.
	{
		char poolMsg[128];
		sprintf(poolMsg, "[pool] boot complete: %lu/%lu paging pages used\n",
		        paging_pool_pages_used(), kPagingPagesCount);
		serial_print_string(poolMsg);
	}
	uint64_t lastPoolReport = kSystemCurrentTime;
	uint64_t lastPoolUsed = paging_pool_pages_used();

	while (true)
	{
		if (lastTime != kSystemCurrentTime)
		{
            if (kSystemCurrentTime % 2 == 0)
                print_at(&kRenderer, 115, 0, heartbeat);
            else
                print_at(&kRenderer, 115, 0, " ");
			lastTime = kSystemCurrentTime;

			// The heartbeat (used to be clock) is a STATUS WIDGET, not a console write. print_at()
			// paints it at a fixed cell without touching the console cursor —
			// so it cannot land in the middle of husk's prompt, and there is no
			// save/moveto/print/restore sequence to race against husk echoing
			// keystrokes on another core. (That race is exactly what the old
			// cursor save/restore had; it just hadn't bitten yet.)

			// Pool slope report, once a minute, serial-direct (same rationale
			// as the boot receipt above). The DELTA is the story: a healthy
			// steady state is +0/min; the address-march bug read as a steady
			// +30ish/min climb toward the exhaustion panic.
			if (kSystemCurrentTime - lastPoolReport >= 60)
			{
				uint64_t used = paging_pool_pages_used();
				char poolMsg[128];
				sprintf(poolMsg, "[pool] %lu/%lu paging pages used (+%lu this minute)\n",
				        used, kPagingPagesCount, used - lastPoolUsed);
				serial_print_string(poolMsg);
				lastPoolReport = kSystemCurrentTime;
				lastPoolUsed = used;
			}
        }
        sigaction(SIGSLEEP, NULL, kTicksSinceStart+90,kKernelTask->threads);
	}
	while (true) {asm("sti\nhlt\n");}
}