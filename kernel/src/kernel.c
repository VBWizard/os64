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
#include "idt.h"
#include "tss.h"
#include "pci.h"
#include "ahci.h"
#include "ata.h"
#include "memset.h"
#include "vfs.h"
#include "acpi.h"
#include "nvme.h"
#include "kernel_commandline.h"
#include "strings.h"
#include "fat_glue.h"
#include "shutdown.h"
#include "tests.h"
#include "panic.h"
#include "task.h"
#include "scheduler.h"
#include "x86_64.h"
#include "smp_core.h"
#include "apic.h"
#include "signals.h"
#include "log.h"

extern block_device_info_t* kBlockDeviceInfo;
extern int kBlockDeviceInfoCount;
extern bool kEnableAHCI;
extern bool kEnableNVME;
bool kEnableSMP;
volatile uint64_t kSystemStartTime;
volatile uint64_t kUptime;
volatile uint64_t kTicksSinceStart;
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
char kRootPartUUID[36] = {0};
vfs_filesystem_t* kRootFilesystem=NULL;
char startTime[100] = {0};
uint64_t lastTime = 0;
task_t* kKernelTask;
uint64_t kCPUCyclesPerSecond;
task_t* kIdleTasks[MAX_CPUS];
task_t* kLogDTask;

/// @brief Create the kernel task
/// This is done manually whereas every other task in the system is created by calling the task_create method in task.c.
void create_kernel_task()
{
	task_t parentTask = {0};
	//The structure of the environment as as follows:
	//*Addr 0: 
	//	Room for 512 8 byte pointers to the environment strings
	//*Addr 4096:
	//	Room for 512 more 8 byte pointers to the remaining environment strings
	//*Addr 8192:
	//  1024 environment strings @ 512 bytes each
	//TODO: Change this to be MMAP'd
	parentTask.envPSize = 0;
	parentTask.envSize = 0;
	parentTask.realEnvp = (char**)allocate_memory_aligned(TASK_ENVIRONMENT_MAX_SIZE);
	parentTask.realEnv = (char*)parentTask.mappedEnvp+(PAGE_SIZE*2);
	parentTask.mappedEnvp = (char**)TASK_ENVP_VIRT;
	parentTask.mappedEnv = (char*)TASK_ENV_VIRT;
	paging_map_pages((uintptr_t*)kKernelPML4v, (uintptr_t)parentTask.mappedEnvp, (uintptr_t)parentTask.realEnvp, TASK_ENVIRONMENT_MAX_SIZE / PAGE_SIZE, PAGE_PRESENT | PAGE_WRITE);
	memset(parentTask.mappedEnvp, 0, TASK_ENVIRONMENT_MAX_SIZE);
	parentTask.envPSize = TASK_ENVIRONMENT_MAX_ENTRIES * sizeof(uintptr_t);
	parentTask.envSize = TASK_ENVIRONMENT_MAX_SIZE - parentTask.envPSize;

	parentTask.mappedEnv = (char*)(parentTask.mappedEnvp + TASK_ENVIRONMENT_DATA_OFFSET);
	((char**)parentTask.mappedEnvp)[0] = parentTask.mappedEnv;
	strncpy(parentTask.mappedEnvp[0], "PATH=/", TASK_MAX_PATH_LEN);

	((char**)parentTask.mappedEnvp)[1] = (char*)(parentTask.mappedEnvp + TASK_ENVIRONMENT_DATA_OFFSET + 8);
	strncpy(parentTask.mappedEnvp[1], "HOSTNAME=yogi.localhost.localdomain", TASK_MAX_PATH_LEN);
	((char**)parentTask.mappedEnvp)[2] = (char*)(parentTask.mappedEnvp + TASK_ENVIRONMENT_DATA_OFFSET + 16);
	strncpy(parentTask.mappedEnvp[2], "CWD=/", TASK_MAX_PATH_LEN);
	parentTask.stdin = STDIN;
	parentTask.stdout = STDOUT;
	parentTask.stderr = STDERR;
	kKernelTask = task_create("ktask", 0, NULL, &parentTask, true, 0);
	scheduler_init();
	scheduler_submit_new_task(kKernelTask);
	mp_CoreHasRunScheduledThread[0] = true;
}

void kernel_init()
{
	printf("Initializing ACPI\n");
	acpiFindTables();
	if (kPCIBaseAddress)
	{
		kPCIBaseAddress = kHHDMOffset | kPCIBaseAddress;
	}

	init_GDT();
	
	logging_queueing_init();


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
	kCPUCyclesPerSecond = tscGetCyclesPerSecond();

	printf("Detected cpu: %s\n", &kcpuInfo.brand_name);
	if (kEnableSMP)
	{
		printf("SMP: Initializing ... ");
		kLimineSMPInfo = smp_request.response;
		init_SMP();
        printf("(%u cores initialized)\n", kMPCoreCount);
    }
	else
		printf("SMP: Disabled due to nosmp parameter\n");
	init_signals();

	create_kernel_task();

	ap_initialization_handler();

	remap_irq0_to_apic(0x20);

    for (int cnt=0;cnt<kMPCoreCount;cnt++)
    {
		char idleTaskName[10];
		sprintf(idleTaskName, "/idle%u",cnt);
		kIdleTasks[cnt] = task_create(idleTaskName, 0, NULL, kKernelTask, true, cnt);
		scheduler_submit_new_task(kIdleTasks[cnt]);
	}

	#if ENABLE_LOG_BUFFERING == 1
    kLogDTask = task_create("/logd", 0, NULL, kKernelTask, true, 0);
	scheduler_submit_new_task(kLogDTask);
	#endif
	scheduler_enable();

	scheduler_change_thread_queue(kKernelTask->threads, THREAD_STATE_RUNNING);
	core_local_storage_t *cls = get_core_local_storage();
	cls->threadID = kKernelTask->threads->threadID;

	mp_enable_scheduling_vector(0);

	wait(1000);

	ap_wake_up_aps();

	kProcessSignals = true;
/*
	if (kRootPartUUID[0])
	{
		printd(DEBUG_BOOT, "BOOT: ROOTPARTUUID passed in commandline.  Will mount '%s' as the root partition\n",&kRootPartUUID);
		vfs_mount_root_part((char*)&kRootPartUUID);
	}

	if (kRootFilesystem!=NULL)
	{
	 	int lResult = testVFS(kRootFilesystem);
	 	if (lResult)
	 		panic("Root filesystem disk test failed: %u\n",lResult);
		kRootFilesystem->fops->uninitialize(kRootFilesystem);
	 }
*/
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

	// Unmask IRQ1 (keyboard) on primary PIC
	uint8_t mask = inb(0x21);
	mask &= ~(1 << 1); // Clear bit 1 (unmask IRQ1)
	outb(0x21, mask);

	kEnableSMP = true;
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
	init_os64_paging_tables();
	kKernelStack = (uintptr_t)kmalloc_aligned(KERNEL_STACK_SIZE);
	__asm__ volatile ("cli\nmov rsp, %0\nsti\n" : : "r" (kKernelStack + KERNEL_STACK_SIZE - 8));
	printf("Kernel stack initialized, 0x%x bytes\n", KERNEL_STACK_SIZE);
	kernel_init();
}
