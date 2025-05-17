#include "CONFIG.h"
#include "serial_logging.h"
#include "allocator.h"
#include "BasicRenderer.h"
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
	char currentTime[100] = {0};
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
	printf("All done, hcf-time!\n");
	printd(DEBUG_EXCEPTIONS,"All done, hcf-time!\n");	
	printf("12345678901234567890123456789012345678901234567890123456789012345678901234567890\n");
	while (true)
	{
		if (lastTime != kSystemCurrentTime)
		{
			uint64_t minutesUptime = (kTicksSinceStart / TICKS_PER_SECOND) / 60; // adjust 1000 if needed
			strftime_epoch(&currentTime[0], 100, "%m/%d/%Y %H:%M:%S", kSystemCurrentTime + (kTimeZone * 60 * 60));
			lastTime = kSystemCurrentTime;
		
			moveto(&kRenderer, 95, 0);
			printf("%s %lu min", currentTime, minutesUptime);
		}
		sigaction(SIGSLEEP, NULL, kTicksSinceStart+49,kKernelTask->threads);
	}
	while (true) {asm("sti\nhlt\n");}
}