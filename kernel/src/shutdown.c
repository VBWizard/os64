#include "CONFIG.h"
#include "serial_logging.h"
#include "allocator.h"
#include "BasicRenderer.h"
#include "strftime.h"

int usedCount=0;
extern volatile uint64_t kSystemCurrentTime;
extern BasicRenderer kRenderer;
extern int kTimeZone;

void shutdown()
{
	uint64_t lastTime = 0;
	char startTime[100] = {0};
	printd(DEBUG_SHUTDOWN, "BOOT END: Status of memory status (%u entries):\n",kMemoryStatusCurrentPtr);
	for (uint64_t cnt=0;cnt<kMemoryStatusCurrentPtr;cnt++)
	{
		printd(DEBUG_SHUTDOWN, "\tMemory at 0x%016Lx for 0x%016Lx (%Lu) bytes is %s\n",kMemoryStatus[cnt].startAddress, kMemoryStatus[cnt].length, kMemoryStatus[cnt].length, kMemoryStatus[cnt].in_use?"in use":"not in use");
		if (kMemoryStatus[cnt].in_use)
			usedCount++;
	}
	printd(DEBUG_SHUTDOWN, "Found %u memory status entries in use, only %u really utilized\n", kMemoryStatusCurrentPtr, usedCount);
	printf("All done, hcf-time!\n");
	printd(DEBUG_BOOT,"All done, hcf-time!\n");	
	printf("12345678901234567890123456789012345678901234567890123456789012345678901234567890\n");
	while (true)
	{
		if (lastTime != kSystemCurrentTime)
		{
			strftime_epoch(&startTime[0], 100, "%m/%d/%Y %H:%M:%S", kSystemCurrentTime + (kTimeZone * 60 * 60));
			lastTime = kSystemCurrentTime;
			moveto(&kRenderer, 80,0);
			printf("%s",startTime);
		}
		asm("sti\nhlt\n");
	}
	while (true) {asm("sti\nhlt\n");}
}