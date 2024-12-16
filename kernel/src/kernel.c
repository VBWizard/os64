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
#include "ata.h"
#include "memset.h"
#include "part_table.h"
#include "vfs.h"
#include "acpi.h"
#include "nvme.h"
#include "kernel_commandline.h"

extern block_device_info_t* kBlockDeviceInfo;
extern int kBlockDeviceInfoCount;
extern bool kEnableAHCI;
extern bool kEnableNVME;

volatile uint64_t kSystemStartTime, kUptime, kTicksSinceStart;
volatile uint64_t kSystemCurrentTime;
int kTimeZone;
volatile bool kInitDone;
volatile bool kFBInitDone = 0;
uint64_t kTicksPerSecond;
struct limine_smp_response *kLimineSMPInfo;
__uint128_t kDebugLevel = 0;
uintptr_t kKernelStack = 0;
char kKernelCommandline[512];

char startTime[100];

void kernel_init()
{
	printf("Initializing ACPI\n");
	acpiFindTables();
	if (kPCIBaseAddress)
	{
		kPCIBaseAddress = kHHDMOffset | kPCIBaseAddress;
	}

	init_GDT();
	
	printf("Initializing PCI: ");
	init_PCI();
	printf("\t%u Busses, %u devices\n",kPCIBridgeCount,kPCIDeviceCount+kPCIFunctionCount);
	if (kEnableAHCI)
	{
		printf("Initializing AHCI ...\n");
		init_AHCI();
	}
	if (kEnableNVME)
	{
		printf("Initializing NVME: ");
		init_NVME();
	}
	detect_cpu();
	printf("Detected cpu: %s\n", &kcpuInfo.brand_name);
	printf("SMP: Initializing ... ");
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

	file_operations_t fileOps;
	for (int idx=0;idx<kBlockDeviceInfoCount;idx++)
	{
		if (kBlockDeviceInfo[idx].ATADeviceType == ATA_DEVICE_TYPE_SATA_HD ||  kBlockDeviceInfo[idx].ATADeviceType == ATA_DEVICE_TYPE_NVME_HD ||  kBlockDeviceInfo[idx].ATADeviceType == ATA_DEVICE_TYPE_HD)
		{
			kBlockDeviceInfo[idx].block_device->partTableType = detect_partition_table_type(&kBlockDeviceInfo[idx]);
			read_block_partitions(&kBlockDeviceInfo[idx]);
/*			switch (kBlockDeviceInfo[cnt].ATADeviceType)
			{
				case ATA_DEVICE_TYPE_SATA_HD:
					 ATA_DEVICE_TYPE_NVME_HD:
					 ATA_DEVICE_TYPE_HD:

			}
			fileOps->initialize = &ext2_initialize_filesystem;
*/		}
	}

	for (int cnt=0;cnt<kBlockDeviceInfoCount;cnt++)
		for (int part=0;part<kBlockDeviceInfo[cnt].block_device->part_count;part++)
			if (kBlockDeviceInfo[cnt].block_device->partition_table->parts[part]->filesystemType == FILESYSTEM_TYPE_EXT2)
			{
				fileOps.initialize = &ext2_initialize_filesystem;
				vfs_block_device_t* t = kRegisterBlockDevice("/", &kBlockDeviceInfo[cnt], part, &fileOps);
				t->fops->initialize(t);
				

			}

	uint64_t addr = paging_walk_paging_table((pt_entry_t*)kKernelPML4v, 0x200000);

    // We're done, just hang...
  
	extern uint64_t kMemoryStatusCurrentPtr;
	extern memory_status_t *kMemoryStatus;
	printd(DEBUG_MEMMAP, "BOOT END: Status of memory status (%u entries):\n",kMemoryStatusCurrentPtr);
	for (uint64_t cnt=0;cnt<kMemoryStatusCurrentPtr;cnt++)
	{
		printd(DEBUG_MEMMAP, "\tMemory at 0x%016Lx for 0x%016Lx (%Lu) bytes is %s\n",kMemoryStatus[cnt].startAddress, kMemoryStatus[cnt].length, kMemoryStatus[cnt].length, kMemoryStatus[cnt].in_use?"in use":"not in use");
	}
	printf("All done, hcf-time!\n");
	printd(DEBUG_MEMMAP,"All done, hcf-time!\n");	
	printf("12345678901234567890123456789012345678901234567890123456789012345678901234567890\n");
	while (true)
	{
		strftime_epoch(&startTime[0], 100, "%m/%d/%Y %H:%M:%S", kSystemCurrentTime + (kTimeZone * 60 * 60));
		moveto(&kRenderer, 80,0);
		printf("%s",startTime);
		asm("sti\nhlt\n");
	}
	while (true) {asm("sti\nhlt\n");}

}

void parse_debug_level(__uint128_t value, uint64_t* high, uint64_t* low)
{
    *high = (uint64_t)(value >> 64);
    *low = (uint64_t)value;
}

void log_debug_level(__uint128_t value) {
	uint64_t high, low;
	parse_debug_level(value, &high, &low);
    printd(DEBUG_BOOT,"DEBUG_OPTIONS 0x%016lx%016lx\n", high, low);
}

//NOTE: The stack is re-loaded in this method, after paging is initialized.  Any method level variables declared will no longer exist after that.
//		Make changes in kernel_init() instead if you need variables.
void kernel_main()
{
	kDebugLevel = DEBUG_OPTIONS;
	kInitDone = false;
	kTicksPerSecond = TICKS_PER_SECOND;

	process_kernel_commandline(kKernelCommandline);
	hardware_init();
	strftime_epoch(&startTime[0], 100, "%m/%d/%Y %H:%M:%S", kSystemCurrentTime + (kTimeZone * 60 * 60));
#ifdef ENABLE_COM1
	init_serial(0x3f8);
#endif
	kKernelPML4v = kHHDMOffset + kKernelPML4;
	init_video(framebuffer_request.response->framebuffers[0], limine_module_response);
	printd(DEBUG_BOOT, "***** OS64 - system booting at %s *****\n", startTime);
	printf(	"***** OS64 - system booting at %s *****\n", startTime);
	uint64_t high, low;
	parse_debug_level(kDebugLevel, &high, &low);
	printf("Commandline: %s (debug level 0x%016lx%016lx)\n",kKernelCommandline, high, low);
	log_debug_level(kDebugLevel);
	printf("Parsing memory map ... %u entries\n",memmap_response->entry_count);
	memmap_init(memmap_response->entries, memmap_response->entry_count);
	printf("Initializing paging (HHMD) ... \n");
	paging_init();
	printf("Initializing allocator, available memory is %Lu bytes\n",kAvailableMemory);
	allocator_init();
	kKernelStack = (uintptr_t)kmalloc_aligned(KERNEL_STACK_SIZE);
	__asm__ volatile ("mov rsp, %0" : : "r" (kKernelStack + KERNEL_STACK_SIZE - 8));
	printf("Kernel stack initialized, 0x%x bytes\n", KERNEL_STACK_SIZE);
	kernel_init();
}
