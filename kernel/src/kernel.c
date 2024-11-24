#include <stddef.h>
#include "kernel.h"
#include "paging.h"
#include "allocator.h"
#include "video.h"
#include "memmap.h"
#include "kmalloc.h"
#include "sprintf.h"
#include "io.h"
#include "serial_logging.h"
#include "init.h"
#include "strftime.h"
#include "driver/system/cpudet.h"
#include "smp.h"
#include "gdt.h"
#include "tss.h"
#include "pci.h"
#include "ahci.h"
#include "strcpy.h"

volatile uint64_t kSystemStartTime, kUptime, kTicksSinceStart;
volatile uint64_t kSystemCurrentTime;
int kTimeZone;

volatile bool kInitDone;
uint64_t kTicksPerSecond;
struct limine_smp_response *kLimineSMPInfo;

void kernel_main()
{
	kInitDone = false;
	kTicksPerSecond = TICKS_PER_SECOND;
	hardware_init();
	char startTime[100];
	strftime_epoch(&startTime[0], 100, "%m/%d/%Y %H:%M:%S", kSystemCurrentTime + (kTimeZone * 60 * 60));
	printd(DEBUG_BOOT, "***** OS64 - system booting at %s *****\n", startTime);
	init_serial(0x3f8);
	kKernelPML4v = kHHDMOffset + kKernelPML4;
	init_video(framebuffer_request.response->framebuffers[0], limine_module_response);
	printf("Parsing memory map ... %u entries\n",memmap_response->entry_count);
	memmap_init(memmap_response->entries, memmap_response->entry_count);
	printf("Initializing paging (HHMD) ... \n");
	paging_init();
	printf("Initializing allocator, available memory is %Lu bytes\n",kAvailableMemory);
	allocator_init();
	init_GDT();
	
	printf("Initializing PCI ...\n");
	init_PCI();
	printf("Initializing AHCI ...\n");
	init_AHCI();
	printf("%u Busses, %u devices\n",kPCIBridgeCount,kPCIDeviceCount+kPCIFunctionCount);
	detect_cpu();
	printf("Detected cpu: %s\n", &kcpuInfo.brand_name);
	printf("SMP: Initializing ...\n");
	init_SMP();
	kLimineSMPInfo = smp_request.response;

	//Temporary - make sure paging is working correctly
	char* x = kmalloc(256);
	char* y = kmalloc(128);

	strncpy(x, "This is test # 1", 20);
	strncpy(y, "this is test # 2", 20);
	kfree(x);
	x = kmalloc(256);
	strncpy(x, "This is test 3", 20);
	
    // We're done, just hang...
    
	extern uint64_t kMemoryStatusCurrentPtr;
	extern memory_status_t *kMemoryStatus;
	printd(DEBUG_BOOT, "BOOT END: Status of memory status:\n");
	for (uint64_t cnt=0;cnt<kMemoryStatusCurrentPtr;cnt++)
	{
		printd(DEBUG_BOOT, "\tMemory at 0x%016Lx for 0x%016Lx (%Lu) bytes is %s\n",kMemoryStatus[cnt].startAddress, kMemoryStatus[cnt].length, kMemoryStatus[cnt].length, kMemoryStatus[cnt].in_use?"in use":"not in use");
	}
	printf("All done, hcf-time!\n");
	printd(DEBUG_BOOT,"All done, hcf-time!\n");
	while (true)
	{
		strftime_epoch(&startTime[0], 100, "%m/%d/%Y %H:%M:%S", kSystemCurrentTime + (kTimeZone * 60 * 60));
		moveto(&kRenderer, 0,20);
		printf("%s",startTime);
	}
	while (true) {asm("sti\nhlt\n");}
}
