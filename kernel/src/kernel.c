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
#include "ata.h"
#include "memset.h"
#include "part_table.h"
#include "vfs.h"
#include "acpi.h"
#include "nvme.h"
#include "kernel_commandline.h"
#include "strings.h"
#include "fat_glue.h"
#include "shutdown.h"

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
bool kOverrideFileLogging;

char startTime[100] = {0};
uint64_t lastTime = 0;

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

	vfs_filesystem_t* testFS;
	bool mounted = false;
	vfs_file_operations_t fileOps;
	vfs_directory_operations_t dirOps;
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
	{
		for (int part=0;part<kBlockDeviceInfo[cnt].block_device->part_count;part++)
		{
			switch (kBlockDeviceInfo[cnt].block_device->partition_table->parts[part]->filesystemType)
			{
				// case FILESYSTEM_TYPE_EXT2:
				// 	fileOps.initialize = &ext2_initialize_filesystem;
				// 	vfs_filesystem_t* t = kRegisterFilesystem("/", &kBlockDeviceInfo[cnt], part, &fileOps);
				// 	mounted = true;
				// 	break;
				case FILESYSTEM_TYPE_FAT32:
					fileOps = fat_fops;
					dirOps = fat_dops;
					testFS = kRegisterFilesystem("/", &kBlockDeviceInfo[cnt], part, &fileOps, &dirOps);
					mounted = true;
					break;
				default: break;
			}
			if (mounted)break;
		}
		if (mounted)break;
	}
	
    // We're done, just hang...
  
	printf("\nDisk tests:\n");
	vfs_file_t* testFile = NULL;
	vfs_directory_t* testDir = NULL;
	FILINFO* fi=kmalloc(sizeof(FILINFO));

	char* contents = kmalloc(4096);

	if (testFS!=NULL)
	{
		if(testFS->fops->open(&testFile, "/fat32_partition", "r", testFS)==0)
		{
			testFS->fops->read(testFile, contents, 4096);
			printf("fat32_partition file contents via read = %s\n",contents);
			testFS->fops->seek(testFile, 0, SEEK_SET);
			memset(contents, 0, 4096);
			testFS->fops->fgets(testFile, contents, 4096);
			printf("fat32_partition file contents via fgets = %s \n",contents);
			testFS->fops->close(testFile);
		}
		if (testFS->dops->open(&testDir, "/", testFS)==0)
		{
			do
			{
				if(!testFS->dops->read(testDir, fi) && fi->fname[0] != '\0')
				{
					if (fi->fattrib & AM_DIR)
						printf("Directory: %s\n",fi->fname);
					else
						printf("File: %s - Attrib 0x%02x - Size %u\n",fi->fname, fi->fattrib, fi->fsize);
				}
			} while (fi->fname[0] != '\0');
		}
		if (testFS->bops->write)
		{
			if (testFS->fops->open(&testFile, "/test2","c", testFS)==0)
			{
				testFS->fops->write(testFile, "Hello world from Chris!\n",24);
				testFS->fops->close(testFile);
				printf("New test file /test2 written\n");
			}
			if(testFS->fops->open(&testFile, "/test2", "r", testFS)==0)
			{
				testFS->fops->fgets(testFile, contents, 4096);
				testFS->fops->close(testFile);
				printf("New file test2 Contents = %s\n",contents);
			}
		}
		else
			printf("Disk %s does not have a write function, skipping write tests\n", testFS->block_device_info->ATADeviceModel);
	}

	kfree(contents);


	shutdown();
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
	pt_entry_t* temp = (pt_entry_t*)kKernelPML4v;

	uintptr_t value = VIRT_TO_PHYS(kKernelPML4v) | PAGE_PRESENT | PAGE_WRITE;
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
	init_os64_paging_tables();
	kKernelStack = (uintptr_t)kmalloc_aligned(KERNEL_STACK_SIZE);
	__asm__ volatile ("mov rsp, %0" : : "r" (kKernelStack + KERNEL_STACK_SIZE - 8));
	printf("Kernel stack initialized, 0x%x bytes\n", KERNEL_STACK_SIZE);
	kernel_init();
}
