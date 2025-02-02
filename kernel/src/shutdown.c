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
	char startTime[100] = {0};
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
			strftime_epoch(&startTime[0], 100, "%m/%d/%Y %H:%M:%S", kSystemCurrentTime + (kTimeZone * 60 * 60));
			lastTime = kSystemCurrentTime;
			moveto(&kRenderer, 80,0);
			printf("%s",startTime);
			moveto(&kRenderer, 60, 2);
			printf("kTicksSinceStart=%u\n", kTicksSinceStart);
			moveto(&kRenderer, 60, 3);
			printf("kSystemCurrentTime-kSystemStartTime=%u\n", kSystemCurrentTime - kSystemStartTime );
			moveto(&kRenderer, 60, 4);
			printf("kTicksSinceStart-(kSystemCurrentTime-kSystemStartTime)*10=%u", kTicksSinceStart-(kSystemCurrentTime - kSystemStartTime) * 10);
		}
		sigaction(SIGSLEEP, NULL, kTicksSinceStart+99,kKernelTask->threads);
		//wait(10);
		// Mask IRQ0 in the PIC (8259A)
		//outb(0x21, inb(0x21) | 0x01); // Mask IRQ0 on the master PIC

	}
	while (true) {asm("sti\nhlt\n");}
}