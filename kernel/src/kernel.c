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

extern block_device_info_t* kATADeviceInfo;
extern int kATADeviceInfoCount;
extern bool kEnableAHCI;
extern bool kEnableNVME;

volatile uint64_t kSystemStartTime, kUptime, kTicksSinceStart;
volatile uint64_t kSystemCurrentTime;
int kTimeZone;
volatile bool kInitDone;
volatile bool kFBInitDone = 0;
uint64_t kTicksPerSecond;
struct limine_smp_response *kLimineSMPInfo;
uint64_t kDebugLevel = 0;
uintptr_t kKernelStack = 0;
char kKernelCommandline[512];

char startTime[100];

void kernel_init()
{
	printd(DEBUG_BOOT, "***** OS64 - system booting at %s *****\n", startTime);
	printf(	"***** OS64 - system booting at %s *****\n", startTime);
	printf("Initializing ACPI\n");
	acpiFindTables();
	if (kPCIBaseAddress)
	{
		printd(DEBUG_BOOT, "Mapping PCI base physical address 0x%016x to the same physical address in the HHMD for 10,000 pages\n", kPCIBaseAddress);
		paging_map_pages((pt_entry_t*)kKernelPML4v, kHHDMOffset | kPCIBaseAddress, kPCIBaseAddress, 0x5000, PAGE_PRESENT | PAGE_WRITE | PAGE_PCD);
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

/*
	unsigned char* buffer = kmalloc(16384);
	buffer = (unsigned char*)((uintptr_t)buffer) - kHHDMOffset;
	paging_map_pages((pt_entry_t*)kKernelPML4v, (uint64_t)buffer, (uint64_t)buffer, 4, PAGE_PRESENT | PAGE_WRITE);
	memset(buffer,0,8192);
	//The first 4 slots are reserved for ATA devices.  The 4th slot is the first slot for AHCI and/or NVME
	ahci_lba_read((void*)&kATADeviceInfo[4], 0,(void*)buffer,2);
	if (buffer[510]!=0x55 || buffer[511] != 0xAA)
		printf("AHCI disk read incorrect.  Expected 0x55, 0xAA, got 0x%02X, 0x%02X\n", buffer[510], buffer[511]);
	else
		printf("AHCI disk read test passed ...\n");

	block_operations_t* ahciBlockOps;
	ahciBlockOps = kmalloc(sizeof(ahciBlockOps));
	ahciBlockOps->read = (void*)&ahci_lba_read;
	kATADeviceInfo[4].block_device->partTableType = detect_partition_table_type(&kATADeviceInfo[4]);
	read_block_partitions(kATADeviceInfo, kATADeviceInfoCount);
	file_operations_t* fileOps;
	fileOps = kmalloc(sizeof(file_operations_t));
	fileOps->initialize = &ext2_initialize_filesystem;
	
	kRegisterFileSystem("/", &kATADeviceInfo[4], 0, fileOps);
*/
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
	while (true)
	{
		strftime_epoch(&startTime[0], 100, "%m/%d/%Y %H:%M:%S", kSystemCurrentTime + (kTimeZone * 60 * 60));
		moveto(&kRenderer, 60,0);
		printf("%s",startTime);
		asm("sti\nhlt\n");
	}
	while (true) {asm("sti\nhlt\n");}

}

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
