#include "CONFIG.h"
#include "serial_logging.h"
#include "allocator.h"
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
        }
        sigaction(SIGSLEEP, NULL, kTicksSinceStart+90,kKernelTask->threads);
	}
	while (true) {asm("sti\nhlt\n");}
}