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
#include "env.h"
#include "gui/compositor.h"
#include "driver/system/mouse.h"
#include "block_device.h"
#include "ramdisk.h"
#include "driver/system/usb/xhci.h"
#include "driver/filesystem/proc/procfs.h"

extern block_device_info_t* kBlockDeviceInfo;
extern int kBlockDeviceInfoCount;
extern bool kEnableAHCI;
extern bool kEnableNVME;
extern bool kEnableRamdisk;
extern bool kRunHello;   // TEMP (userland bring-up) — remove with the launch block below
extern bool kRunKeytest; // TEMP (read-syscall bring-up) — remove with keytest
extern bool kRunHusk;    // launch the shell from the boot flow
extern bool kTestPanic;  // TESTPANIC: deliberately panic post-tests (panic-pipeline diagnostic)
bool kEnableSMP = true;
// The scheduler mode, DEFAULT TRUE since 2026-08-05 (Chris's ruling, decision
// recorded): tickless is the destination architecture (see SCHEDULER_REDESIGN
// on the net branch) and this park-and-nudge mode is the road to it — idle AP
// timers stay masked and work arrives by directed IPI. The name is deliberately
// aspirational: the BSP still ticks at 100Hz and busy APs don't preempt yet;
// the mode grows into its name phase by phase. This flag was born as the
// misnamed BSPSCHED cmdline option (the BSP neither owned the nudging nor the
// scheduling — any core nudges, every core self-schedules), retired same day.
// SCHED=periodic is the only way off: the legacy every-core-100Hz mode, kept
// as a diagnostic flashlight AND as the repro for the open /idle2 stray-write
// (SCHEDULER_STRAY_WRITE.md) until that bug dies.
bool kTicklessScheduler = true;
bool kEnableKWorker = false;
// Cleared by the NOUSB cmdline flag — skips xHCI bring-up entirely.
bool kEnableUSB = true;
// Cleared by the NOTESTS cmdline flag to skip ALL test execution (pre-boot,
// post-boot, and the disk/VFS tests) — used to isolate a boot hang by booting
// with no test code in the path.
bool kRunTests = true;
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
// The TZ= kernel cmdline string, verbatim (classic V7/POSIX format, e.g.
// EST5EDT). Two consumers: init.c derives kTimeZone's standard offset from
// it, and kernel_init seeds it into the first task's environment — where
// libos64's calendar applies the full policy (DST included). The kernel
// itself never learns DST; it just delivers the string.
char kTZString[64] = {0};
vfs_filesystem_t* kRootFilesystem=NULL;
char startTime[100] = {0};
uint64_t lastTime = 0;
task_t* kKernelTask;
uint64_t kCPUCyclesPerSecond;
// Boot TSC calibration window, seconds (TSCCAL= on the cmdline).
//
// Default 5 (was 15 until 2026-08-01). The accuracy argument for 15 was
// real but small: ±1 tick of boundary slop across 1500 ticks is ±0.07%,
// versus ±0.2% across 500. What tipped it is that the continuous
// recalibrator erases that gap within seconds of boot, while the 15
// seconds are paid IN FULL on every single boot, by a human, watching a
// progress line — and this OS gets booted dozens of times an evening.
// Ten seconds of somebody's life beats 0.13% of initial timer accuracy
// that a background task is about to fix anyway. Raise it for a session
// that genuinely needs a tight cold start: TSCCAL=15.
int kTSCCalibrationSeconds = 5;
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
	parentTask.env = env_create();
	// Where husk (and anything else consulting the environment) looks for
	// programs — colon-separated, walked by userland PATH search. Every task
	// inherits this block, so one seed here reaches the whole tree.
	env_set(parentTask.env, "PATH",     "/bin");
	env_set(parentTask.env, "HOSTNAME", "yogi.localhost.localdomain");
	// TZ, if the boot cmdline provided one — the kernel is just the courier
	// here: the string rides into the first task's environment, inheritance
	// carries it to everything husk ever spawns, and libos64's calendar is
	// what actually reads it (offset AND daylight-saving policy). No TZ on
	// the cmdline = no TZ in the env = the library falls back to the
	// kernel's standard offset via the time() syscall. Three tiers, each
	// dumber than the one above it.
	if (kTZString[0])
		env_set(parentTask.env, "TZ", kTZString);
	// Deliberately NO "CWD" here: the kernel owns the real cwd (task->cwd,
	// validated at every chdir) and husk's $CWD expansion asks it live via
	// getcwd. An env-block copy could only ever go stale — it seeded "/" at
	// boot and nothing updated it on chdir. Unix's $PWD is exactly that
	// hand-maintained cache (drift and all); os64 declines to keep it.
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
	// The shared block-device table must exist before ANY storage driver
	// registers into it. (It used to be allocated inside init_AHCI(), which
	// left noahci boots with a NULL table for NVMe to scribble through.)
	init_block();

	// Register the RAMDisk FIRST: the module is a byte-for-byte copy of the
	// QEMU NVMe disk image, so a physical disk carrying the same partition
	// GUID can coexist with it — and the UUID scan in vfs_mount_root_part()
	// takes the first match, which must be the RAMDisk when the boot entry
	// asked for it.
	if (kEnableRamdisk)
	{
		if (kRamdiskModuleAddress != NULL)
		{
			printf("Initializing RAMDisk (%lu MB from boot module)\n", kRamdiskModuleSize / (1024 * 1024));
			init_ramdisk_block_device(kRamdiskModuleAddress, kRamdiskModuleSize);
		}
		else
			printf("RAMDISK flag passed but no os64_disk.img module found; ignoring\n");
	}

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
	// The pause has a NAME on the glass: a silent 15-second stare at a
	// counter reads as a hang to anyone watching a boot.
	printf("Calibrating TSC (%d second window) ... ", kTSCCalibrationSeconds);
	kCPUCyclesPerSecond = tscGetCyclesPerSecond((uint32_t)kTSCCalibrationSeconds);
	printf("%lu cycles/sec\n", kCPUCyclesPerSecond);

	printf("Detected cpu: %s\n", &kcpuInfo.brand_name);

	// USB last among the bus drivers: the P5 has no PS/2 port, so this is
	// where its keyboard comes from. Runs BEFORE task creation on purpose —
	// the xHCI MMIO mapping lands in the kernel PML4's upper half and every
	// task PML4 clones those entries at birth (see xhci.c).
	if (kEnableUSB)
	{
		printf("Initializing USB (xHCI): ");
		init_xHCI();
	}
    printf("SMP: Initializing ... ");
    kLimineSMPInfo = smp_request.response;
    init_SMP(kEnableSMP);
    printf("(%u core(s) initialized)\n", kMPCoreCount);
    // Say which scheduler this boot got — the mode is a cmdline decision now
    // (tickless default, SCHED=periodic opt-out), and a decision that changes
    // how every core behaves should be readable on the glass, not inferred.
    printf("Scheduler: %s\n", kTicklessScheduler
           ? "tickless (park-and-nudge; SCHED=periodic for legacy)"
           : "periodic (legacy 100Hz all-core; diagnostic mode)");

    init_signals();

	create_kernel_task();

	ap_initialization_handler();

	remap_irq0_to_apic(0x20);

	// Keyboard IRQ1 rides the IOAPIC too — unconditionally, GUI or not.
	// remap_irq0_to_apic just switched the IMCR to APIC mode, which cuts the
	// legacy PIC's INTR wire; PIC-delivered IRQ1 only kept working where
	// firmware left LINT0 in virtual-wire mode (QEMU yes, VBox/real HW not
	// guaranteed). Same vector (0x21), same handler — only the delivery path
	// and EOI target change.
	//
	// Destination: with the GUI running, input IRQs are aimed at the
	// COMPOSITOR's core so a keystroke or mouse packet wakes its hlt-wait
	// immediately (input latency without a busy-wait — see guicomp_thread's
	// pacing). Text-mode boots keep them on the BSP.
	// Vectors 0x41/0x4C, not the legacy 0x21/0x2C: AP LAPICs run with
	// TPR = 0x30, which masks all vectors below 0x40 (see idt.c).
	uint8_t inputIrqDest = kCPUInfo[0].apicID;
	if (kEnableGUI && gui_compositor_affinity() != THREAD_NO_AFFINITY)
		inputIrqDest = (uint8_t)gui_compositor_affinity();
	ioapic_adopt_isa_irq(1, 0x41, inputIrqDest, &kIRQ1UsesLapic);

	// The mouse only matters to the GUI; a text-mode boot skips the whole
	// AUX-port bring-up (and IRQ12 stays dormant — its IDT entry exists but
	// nothing routes to it).
	if (kEnableGUI)
	{
		mouse_init();
		ioapic_adopt_isa_irq(12, 0x4C, inputIrqDest, &kIRQ12UsesLapic);
	}

    // We need the cls->task to be populated for running tests, so ...
    // put the kernel task in the cls because it'll be the first task to start running
    get_core_local_storage()->task = kKernelTask;
    kKernelTask->pml4 = (pt_entry_t*)kKernelPML4;
    kKernelTask->pml4v = (pt_entry_t*)kKernelPML4v;
    // Init and run tests before configuring and enabling the scheduler
    // (skippable via the NOTESTS cmdline flag).
    if (kRunTests)
    {
        test_framework_init();
        printf("Running pre-boot tests ... ");
        test_run_preboot();
        printf(" done\n");
    }

	// BOOTMARK mile-markers (kernel.h) — PERMANENT instrumentation, gated by
	// the BOOTMARKS cmdline flag. Born hunting the 54-second VBox boot
	// (2026-07-11); kept because per-phase tick+TSC deltas answer "where did
	// boot time go" in one run.
	BOOTMARK("preboot-done");

        for (int cnt = 0; cnt < kMPCoreCount; cnt++)
	    {
			char idleTaskName[10];
			sprintf(idleTaskName, "/idle%u",cnt);
			kIdleTasks[cnt] = task_create(idleTaskName, 0, NULL, kKernelTask, true, kCPUInfo[cnt].apicID);
			scheduler_submit_new_task(kIdleTasks[cnt]);
		}

	BOOTMARK("idle-tasks-created");

	#if ENABLE_LOG_BUFFERING == 1
	    kLogDTask = task_create("/logd", 0, NULL, kKernelTask, true, THREAD_NO_AFFINITY);
	    // Pass daemon=true (first arg in RDI) to logd_thread
	    kLogDTask->threads->regs.RDI = 1;
	    scheduler_submit_new_task(kLogDTask);
	#endif
	BOOTMARK("logd+kworker-created");

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
	BOOTMARK("scheduler-enabled");

	mp_enable_scheduling_vector(0);
	BOOTMARK("bsp-sched-vector-on");

	wait(1000);
	BOOTMARK("wait1000-done");

	ap_wake_up_aps();
	BOOTMARK("aps-awake");

	kProcessSignals = true;
	BOOTMARK("signals-on");


    printd(DEBUG_BOOT, "BOOT: If a ROOTPARTUUID (%s) was passed in the commandline, we'll load it.\n", &kRootPartUUID);
	if (kRootPartUUID[0])
	{
		printd(DEBUG_BOOT, "BOOT: ROOTPARTUUID passed in commandline.  Will mount '%s' as the root partition\n",&kRootPartUUID);
		vfs_mount_root_part((char*)&kRootPartUUID);
	}

	// /proc — processes as files (PROC.md). Mounted AFTER the root and the
	// secondary-partition sweep for two reasons: the mount table is walked
	// longest-prefix-first so order does not affect routing, but procfs has no
	// partition GUID at all, and letting the sweep finish first keeps its
	// all-zero GUID out of the dedupe comparisons. It also needs no filesystem
	// underneath it — this mount works even if no disk was found.
	procfs_mount();

	// ── The log daemon, as early as a log daemon can possibly start ──────────
	// HERE, and not down with husk, is the whole point of the LOGD= flag. The
	// expensive part of a DEBUG_DETAILED boot is everything BELOW this line —
	// driver bring-up, the test suite, the shell — and a daemon started after
	// all that would have automated away the typing while leaving the cost.
	// This is the first instruction in the boot where a filesystem exists to
	// exec from, so it is the earliest honest answer.
	//
	// Nothing is lost by starting late anyway: the kernel has been queueing
	// into the per-core rings since boot and (with LOGD= set) deliberately not
	// draining them to serial, so logd's first read collects the ENTIRE boot,
	// first line included, and writes it to the file at memcpy speed.
	if (kLogdPath[0] && kRootFilesystem != NULL)
	{
		printf("Launching /bin/logd -> %s ...\n", kLogdPath);
		char *logdArgv[] = { "/bin/logd", kLogdPath };
		task_t *logdTask = task_create("/bin/logd", 2, logdArgv, kKernelTask, false, THREAD_NO_AFFINITY);
		if (logdTask)
			scheduler_submit_new_task(logdTask);
		else
		{
			// Say so on the glass: with LOGD= set the kernel is holding serial
			// output back for a daemon that now cannot arrive, and the wait's
			// own deadline is the only thing that will notice. Better to name
			// it here than to leave a silent 30 seconds.
			printf("  /bin/logd launch failed (not on the image?) — serial logging resumes shortly\n");
		}
	}

	// TEMP (userland bring-up, remove when the shell exists): launch
	// /bin/hello as a FIRST-CLASS scheduled application from the normal boot
	// flow — task_create + scheduler_submit_new_task, then walk away. No test
	// harness, no exited-polling: the scheduler picks it up on some core, it
	// runs at CPL 3, prints, and exits entirely on its own. This is the exact
	// shape the shell launcher will take (submit /bin/shell, don't babysit).
	// Gated on the HELLO cmdline flag. The brief wait only paces the boot log
	// so hello's output lands in order — the app runs via the scheduler, not
	// via this wait.
	if (kRunHello && kRootFilesystem != NULL)
	{
		printf("Launching /bin/hello as a userland application ...\n");
		task_t *helloTask = task_create("/bin/hello", 0, NULL, kKernelTask, false, THREAD_NO_AFFINITY);
		if (helloTask)
		{
			scheduler_submit_new_task(helloTask);
			// Let the scheduler run it on its own for a beat, then report the
			// outcome to serial (hello's own os64_puts output lands on the
			// framebuffer console, not here). exited+retVal==0 = it ran at
			// CPL 3 and exited cleanly, all via the scheduler.
			wait(1000);
			printf("  /bin/hello: %s (retVal=0x%lx)\n",
			       helloTask->exited ? "ran and exited via the scheduler" : "did NOT exit in 1s",
			       helloTask->retVal);
		}
		else
			printf("  /bin/hello launch failed (not on the image?)\n");
	}

	// TEMP (read-syscall bring-up, remove with keytest): launch /bin/keytest
	// and PARK the kernel task forever so the scheduler keeps running — keytest
	// blocks in read(0) waiting for keys, so the system must stay alive. Skips
	// the tests/shutdown path on purpose; this is a dedicated input-demo boot.
	if (kRunKeytest && kRootFilesystem != NULL)
	{
		printf("Launching /bin/keytest (system stays up for keyboard input) ...\n");
		task_t *ktTask = task_create("/bin/keytest", 0, NULL, kKernelTask, false, THREAD_NO_AFFINITY);
		if (ktTask)
			scheduler_submit_new_task(ktTask);
		else
			printf("  /bin/keytest launch failed (not on the image?)\n");
		while (1)
			sigaction(SIGSLEEP, NULL, kTicksSinceStart + 5 * TICKS_PER_SECOND, kKernelTask->threads);
	}

    if (kRunTests)
    {
        printf("Running post-boot tests ...\n");
        test_run_postboot();
    }

	// (The boot-time testVFS() block lived here from the beginning — outside
	// the framework, panicking on failure. Its write/mkdir half now lives in
	// the suite as test_vfs_write_mkdir, read-only-aware like everything
	// else; its read half was long since covered by the ring-3 fixtures.
	// Removed 2026-07-19 with tests.c itself — the day the framework
	// finished eating its ancestor.)

	// The standing panic-pipeline diagnostic (see kTestPanic's comment in
	// kernel_commandline.c). Placed HERE, after the full boot + test flow, so
	// the log queues carry a realistic backlog for the emergency flush to
	// prove itself against.
	if (kTestPanic)
		panic("TESTPANIC: deliberate panic — if you can read this in the serial log, the panic pipeline works\n");

    // Launch the shell and park the kernel task so the scheduler keeps running
    // it (husk loops forever on read/spawn/wait). The kernel task is husk's
    // parent, so husk inherits its environment. (When husk becomes the real
    // init path this replaces the HELLO/KEYTEST temps entirely.)
    if (kRunHusk && kRootFilesystem != NULL)
    {
        printf("Launching /bin/husk (the shell) ...\n");
        task_t *huskTask = task_create("/bin/husk", 0, NULL, kKernelTask, false, THREAD_NO_AFFINITY);
        if (huskTask)
        {
            // Seat the shell at the console. controllingShell is Ctrl+C
            // immunity (an ETX with the shell foreground stays a DATA byte —
            // husk line-kills; it is never SIGINTed); kForegroundTask is who
            // Ctrl+C aims at, and task_wait moves it to whichever child husk
            // blocks on. Both set BEFORE the task can run. (SIGINT.md)
            huskTask->controllingShell = true;
            kForegroundTask = huskTask;
            scheduler_submit_new_task(huskTask);
        }
        else
            printf("  /bin/husk launch failed (not on the image?)\n");
    }
    
    // The GUI is strictly optional (DOS/Windows relationship): without the GUI
    // cmdline flag we run tests and shut down exactly as before. With it, the
    // desktop owns the machine — start the compositor and park the kernel task
    // in a sleep loop (1s naps, immediately re-armed; all real work happens in
    // the compositor and app tasks).
    if (kEnableGUI)
	{
		printf("Starting GUI ...\n");
		gui_start();
		// Park with a periodic status line: once the console window attaches
		// (kConsoleSink), these printfs flow into the desktop — living proof
		// the print_n diversion works end to end.
		uint64_t statusCount = 0;
		while (1)
		{
			sigaction(SIGSLEEP, NULL, kTicksSinceStart + 5 * TICKS_PER_SECOND, kKernelTask->threads);
			printf("os64: up %lu ticks, GUI running (status #%lu)\n", kTicksSinceStart, ++statusCount);
		}
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

	// The commandline tokenizer NUL-splits its input IN PLACE — hand it a
	// scratch copy so kKernelCommandline itself stays printable. (The
	// "Commandline:" boot line used to show only "ROOT=..." because by print
	// time the parser had already replaced every space with a NUL — the
	// flags weren't missing from the boot, just eaten from the string.)
	{
		static char cmdline_scratch[512];
		strncpy(cmdline_scratch, kKernelCommandline, sizeof(cmdline_scratch));
		cmdline_scratch[sizeof(cmdline_scratch) - 1] = '\0';
		process_kernel_commandline(cmdline_scratch);
	}
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
    printd(DEBUG_BOOT, "Commandline: %s", kKernelCommandline);
    log_debug_level(kDebugLevel);
    printf("Parsing memory map ... %u entries\n",memmap_response->entry_count);
	memmap_init(memmap_response->entries, memmap_response->entry_count);
	printf("Initializing paging (HHMD) ... \n");
	paging_init();
	printf("Initializing allocator, available memory is %Lu bytes\n",kAvailableMemory);
	allocator_init();
	// PAT entry 7 = write-combining, BEFORE the kernel tables are built —
	// init_os64_paging_tables maps the framebuffer PAGE_WC (PAT index 7),
	// and the entry must mean WC before the first store lands through it.
	// Each AP does the same for itself during bring-up (smp_core.c).
	pat_init_this_core();
	init_os64_paging_tables();
	kKernelStack = (uintptr_t)kmalloc_aligned(KERNEL_STACK_SIZE);
	__asm__ volatile ("cli\nmov rsp, %0\nsti\n" : : "r" (kKernelStack + KERNEL_STACK_SIZE - 8));
	printf("Kernel stack initialized, 0x%x bytes\n", KERNEL_STACK_SIZE);
	// kmalloc works now — give the console its RAM shadow so scrolling stops
	// READING VRAM (fine in QEMU's RAM framebuffer, ~2 lines/second on the
	// P5's real write-combined VRAM; see scroll_framebuffer_full).
	renderer_attach_shadow();
	kernel_init();
}
