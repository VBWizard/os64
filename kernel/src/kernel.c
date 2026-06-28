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
#include "test_framework.h"
#include "panic.h"
#include "task.h"
#include "scheduler.h"
#include "x86_64.h"
#include "smp_core.h"
#include "apic.h"
#include "signals.h"
#include "log.h"
#include "exceptions.h"
#include "kworker.h"

extern block_device_info_t* kBlockDeviceInfo;
extern int kBlockDeviceInfoCount;
extern bool kEnableAHCI;
extern bool kEnableNVME;
bool kEnableSMP = true;
bool kBspSchedulerMode = false;
bool kEnableKWorker = false;
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
char kRootPartUUID[37] = {0};
vfs_filesystem_t* kRootFilesystem=NULL;
char startTime[100] = {0};
uint64_t lastTime = 0;
task_t* kKernelTask;
uint64_t kCPUCyclesPerSecond;
task_t* kIdleTasks[MAX_CPUS];
task_t* kLogDTask;
task_t* kKWorkerTask;
task_t parentTask = {0};

/// @brief Create the kernel task
/// This is done manually whereas every other task in the system is created by calling the task_create method in task.c.
void create_kernel_task()
{
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
	kKernelTask = task_create("ktask", 0, NULL, &parentTask, true, THREAD_NO_AFFINITY);
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
    printf("SMP: Initializing ... ");
    kLimineSMPInfo = smp_request.response;
    init_SMP(kEnableSMP);
    printf("(%u core(s) initialized)\n", kMPCoreCount);

    init_signals();

	create_kernel_task();

	ap_initialization_handler();

	remap_irq0_to_apic(0x20);

    // We need the cls->task to be populated for running tests, so ...
    // put the kernel task in the cls because it'll be the first task to start running
    get_core_local_storage()->task = kKernelTask;
    kKernelTask->pml4 = (pt_entry_t*)kKernelPML4;
    kKernelTask->pml4v = (pt_entry_t*)kKernelPML4v;
    // Init and run tests before configuring and enabling the scheduler
    test_framework_init();
    test_run_preboot();

	    for (int cnt = 0; cnt < kMPCoreCount; cnt++)
	    {
			char idleTaskName[10];
			sprintf(idleTaskName, "/idle%u",cnt);
			kIdleTasks[cnt] = task_create(idleTaskName, 0, NULL, kKernelTask, true, kCPUInfo[cnt].apicID);
			scheduler_submit_new_task(kIdleTasks[cnt]);
		}

	#if ENABLE_LOG_BUFFERING == 1
	    kLogDTask = task_create("/logd", 0, NULL, kKernelTask, true, THREAD_NO_AFFINITY);
	    // Pass daemon=true (first arg in RDI) to logd_thread
	    kLogDTask->threads->regs.RDI = 1;
	    scheduler_submit_new_task(kLogDTask);
	#endif

	if (kEnableKWorker && kMPCoreCount > 1)
	{
	    kKWorkerTask = task_create("/kworker1", 0, NULL, kKernelTask, true, kCPUInfo[1].apicID);
	    kKWorkerTask->threads->regs.RDI = 1;
	    kKWorkerTask->autoReap = true;
	    printd(DEBUG_TASK | DEBUG_DETAILED,
	    	"kernel_init: enabling /kworker1 on APIC %u (task=0x%08x, thread=0x%08x)\n",
	    	kCPUInfo[1].apicID,
	    	kKWorkerTask->taskID,
	    	kKWorkerTask->threads->threadID);
	    scheduler_submit_new_task(kKWorkerTask);
	}
	   
	    scheduler_enable();
    scheduler_change_thread_queue(kKernelTask->threads, THREAD_STATE_RUNNING);
    core_local_storage_t *cls = get_core_local_storage();
	cls->threadID = kKernelTask->threads->threadID;

	mp_enable_scheduling_vector(0);

	wait(1000);

	ap_wake_up_aps();

	kProcessSignals = true;

    printd(DEBUG_BOOT, "BOOT: If a ROOTPARTUUID (%s) was passed in the commandline, we'll load it.\n", &kRootPartUUID);
	if (kRootPartUUID[0])
	{
		printd(DEBUG_BOOT, "BOOT: ROOTPARTUUID passed in commandline.  Will mount '%s' as the root partition\n",&kRootPartUUID);
		vfs_mount_root_part((char*)&kRootPartUUID);
	}

    test_run_postboot();

	if (kRootFilesystem!=NULL)
	{
        // Temporary: run a tiny kernel-mode ELF to validate the loader.
		uint64_t faults_before = kPageFaultCount;
        task_t *testElfTask = task_create("/bin/test_elf", 0, NULL, kKernelTask, true, THREAD_NO_AFFINITY);
        scheduler_submit_new_task(testElfTask);
		for (int tries = 0; tries < 50 && kPageFaultCount == faults_before; tries++) {
			wait(100);
		}
		if (kPageFaultCount == faults_before) {
			panic("ELF loader test did not fault in any pages within timeout\n");
		}

		// Test: Verify task_exit() puts thread in zombie queue
		printd(DEBUG_TESTS, "Checking zombie queue for test_elf (no wait)...\n");
		// Test with no wait - see if test_elf is already in zombie queue
		for (int tries = 0; tries < 0; tries++) {
			wait(100);
		}
		printd(DEBUG_TESTS, "Checking zombie queue...\n");

		// Check zombie queue for test_elf's thread
		extern thread_t *qZombie;
		bool found_in_zombie = false;
		thread_t *zombie = qZombie;
		while (zombie != NO_THREAD && zombie != NULL) {
			task_t *zombie_task = (task_t*)zombie->ownerTask;
			if (zombie_task == testElfTask) {
				found_in_zombie = true;
				break;
			}
			zombie = zombie->next;
		}

		if (found_in_zombie) {
			printd(DEBUG_TESTS, "\t[Test] task_exit_zombifies... OK\n");
		} else {
			panic("[Test] task_exit_zombifies... FAIL - test_elf not found in zombie queue\n");
		}

        int lResult = testVFS(kRootFilesystem);
        if (lResult)
	 		panic("Root filesystem disk test failed: %u\n",lResult);
		kRootFilesystem->fops->uninitialize(kRootFilesystem);
	 }

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
