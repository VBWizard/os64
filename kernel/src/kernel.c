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
#include "nmi_probe.h"
#include "symbols.h"
#include "watchpoint.h"   // watchpoint_init — WATCH= arms before the drivers come up
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
#include "tty.h"   // tty_init + tty_seat_shell — the terminals rise at boot
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
#include "driver/block/block_cache.h"   // the buffer cache — between the filesystems and the disks
#include "driver/system/usb/xhci.h"
#include "driver/net/virtio_net.h"
#include "driver/net/e1000.h"
#include "driver/net/r8125.h"
#include "driver/net/ethernet.h"   // init_net_stack — the protocol stack over the seam
#include "driver/net/ipv4.h"       // kNetIPString — the "was IP= given?" DHCP election
#include "driver/net/dhcp.h"
#include "driver/filesystem/proc/procfs.h"
#include "driver/filesystem/sys/sysfs.h"
#include "driver/filesystem/dev/devfs.h"

extern block_device_info_t* kBlockDeviceInfo;
extern int kBlockDeviceInfoCount;
extern const char kBuildStamp[];   // minted at LINK time (kernel/GNUmakefile's
                                   // buildstamp.c) — names THIS binary, so the
                                   // banner proves which kernel actually booted
                                   // (the net-delivered-kernel receipt)
extern bool kEnableAHCI;
extern bool kEnableNVME;
extern bool kEnableRamdisk;
extern bool kRunHello;   // TEMP (userland bring-up) — remove with the launch block below
extern bool kRunKeytest; // TEMP (read-syscall bring-up) — remove with keytest
extern bool kRunHusk;    // launch the shell from the boot flow
extern bool kRunTestrun; // launch /bin/testrun, the ring-3 half of the suite
extern bool kTestPanic;  // TESTPANIC: deliberately panic post-tests (panic-pipeline diagnostic)
extern bool kTestNmiProbe;  // NMIPROBE: sweep every core with a diagnostic NMI post-tests
extern bool kTestPageFault; // TESTPF: deliberate wild-kernel-pointer #PF post-tests
extern bool kTestGPFault;   // TESTGP: deliberate non-canonical-pointer #GP post-tests
extern bool kTestWatchpoint; // TESTWATCH: prove the hardware-watchpoint path post-tests
extern bool kTestShutdown;  // SHUTDOWNTEST: run the full shutdown descent post-tests
bool kEnableSMP = true;
// The scheduler mode, DEFAULT TRUE since 2026-08-05 (Chris's ruling, decision
// recorded): tickless is the destination architecture (see SCHEDULER_REDESIGN
// on the net branch) and this park-and-nudge mode is the road to it — idle AP
// timers stay masked and work arrives by directed IPI. The name is deliberately
// aspirational: the BSP still ticks at 100Hz and busy APs don't preempt yet;
// the mode grows into its name phase by phase. This flag was born as the
// misnamed BSPSCHED cmdline option (the BSP neither owned the nudging nor the
// scheduling — any core nudges, every core self-schedules), retired same day.
// SCHED=periodic is the only way off: the legacy every-core-100Hz mode. Its
// original second job — being the repro for the /idle2 stray-write — ENDED
// 2026-08-03 when that bug was solved (tempStack overflow; the doc now reads
// SOLVED). It has since inherited a better one, and this is the note that says
// so: periodic is the ONLY mode that drives the AP timer-interrupt path hard,
// and as of 2026-08-09 that path is throwing random-looking exceptions again
// (Chris, testing GUI and periodic non-GUI — NOT the old stray write, which is
// dead). That matters far beyond periodic itself: the planned AP preemption
// backstop re-enters the SAME handle_mp_isr -> save -> iretq path, just at a
// lower rate. A rare backstop would not create that bug, it would INHERIT it —
// and a fault once an hour is far worse to chase than one every boot. So
// periodic is not legacy baggage to be parked; it is the fast reproducer for
// the path the next scheduler slice is built on. Retire it as a MODE ANYTHING
// DEPENDS ON (the GUI entries' SCHED=periodic workaround dies with the
// backstop) — keep it forever as the flashlight.
bool kTicklessScheduler = true;
// The AP preemption backstop the block above foretold (2026-08-13): under
// tickless, every non-idle dispatch on an AP arms a one-shot LAPIC lease;
// idle dispatch disarms it. Kills the tickless-AP starvation debt (a
// syscall-free hog could hold a core forever, starving pinned kworker and
// Ctrl+C's forced-syscall redirect). NOBACKSTOP on the cmdline restores the
// pre-backstop tickless exactly — the flashlight rule, same as
// SCHED=periodic/NOCACHE/NOTRACE. Meaningless outside tickless mode.
bool kSchedBackstopEnabled = true;
// The lease length, milliseconds. SCHED_BACKSTOP_MS (CONFIG.h) is the
// compiled DEFAULT; BACKSTOP=<ms> on the cmdline overrides it per boot —
// compile-time default, boot-time policy (his call, 2026-08-13, after the
// GUI shakedown: 50 is plenty for shell work, but the compositor sharing
// its core at 50ms granularity animates like a slideshow — at 10ms it is
// smooth. The GUI entries pass BACKSTOP=10; everything else rides the
// default). Validated after parse: 1..1000 or it snaps back to the default,
// loudly — a typo must not land you at a 0ms lease (= a disarmed timer).
int kSchedBackstopMS = SCHED_BACKSTOP_MS;
bool kEnableKWorker = false;
// Cleared by the NOUSB cmdline flag — skips xHCI bring-up entirely.
bool kEnableUSB = true;
// Set by USBQUIET: unpower the ports of idle xHCI controllers after
// enumeration, to hush the 2.4GHz hash USB3 signaling radiates at wireless
// dongles (the Intel 2012 whitepaper problem; measured on the P5 as a
// 10-feet-under-Windows / 6-inches-under-os64 mouse). DEFAULT OFF, and the
// caution is twice-earned: the per-port version browned out both dongles
// (a USB3 connector is TWO xHCI ports sharing VBUS, and the "empty" twin
// got doused), and on the P5's AMD Rembrandt platform — FIVE controllers:
// two USB3.1 (1022:161d/161e), one USB2-only (161f), two USB4-side
// (15d6/15d7) — a connector's twins may live on DIFFERENT controllers, so
// even whole-idle-controller dousing can cut a live connector's power.
// Until the Supported Protocol capability walk can prove which ports are
// safe, this stays an opt-in flashlight, the NOCACHE/NOBACKSTOP pattern.
bool kUSBQuiet = false;
// Cleared by the NONET cmdline flag — skips NIC bring-up (NETWORK.md arc).
bool kEnableNet = true;
// Cleared by NOR8125 — skips RTL8125 bring-up. The P5 is the only machine
// here that has one, so this flag exists for the day that machine needs to
// boot WITHOUT its NIC in order to diagnose something else.
bool kEnableR8125 = true;
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
	// The environment is an envpage_t (abi/include/os64/env.h): one packed
	// key\0val\0 block, born one page and grown on demand by setenv up to
	// TASK_ENV_MAX_BYTES. (A comment here used to describe a 1024-pointer-slot
	// layout from the pre-envpage design — that structure never existed in
	// os64; the constants describing it left task.h 2026-08-14.)
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
	// Symbols first: everything after this line panics with NAMES. Needs only
	// kmalloc and the kernel-file mapping init_os64_paging_tables carried
	// over, both long since ready; failure is announced and costs nothing
	// but hex addresses (symbols.h has the doctrine).
	symbols_init();

	// Debug registers next, and deliberately THIS early: a WATCH= on the
	// commandline exists to catch something that happens during boot as
	// readily as something that happens an hour in, and every subsystem
	// initialized below this line is a subsystem a watchpoint can now watch.
	// (Symbols first, though — a call chain of bare hex addresses is a riddle,
	// not a report.)
	watchpoint_init();

	printf("Initializing ACPI\n");
	acpiFindTables();
	// Read \_S5 out of the DSDT NOW, while the tables are mapped and the
	// machine is healthy — the shutdown descent is the worst possible moment
	// to be walking firmware tables for the first time (acpi.h).
	acpi_prepare_s5();
	if (kPCIBaseAddress)
	{
		kPCIBaseAddress = kHHDMOffset | kPCIBaseAddress;
	}

	init_GDT();

	// (logging_queueing_init() used to live here. It moved up into
	// kernel_main, next to the kernel stack, so the rings exist before the
	// boot has anything interesting to say — see the block comment there.)


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

	// The buffer cache goes up AFTER the storage drivers have registered
	// their devices and ops (it interposes on those ops in place) and
	// BEFORE anything mounts (though mount order doesn't actually matter —
	// every holder shares the driver's one ops struct). Chris's pattern,
	// Thompson's 1975 idea: the cache lives between the filesystems and
	// the disks, and neither side can tell it's there.
	block_cache_init();
	block_cache_attach_all();

	detect_cpu();
	// The pause has a NAME on the glass: a silent 15-second stare at a
	// counter reads as a hang to anyone watching a boot.
	printf("Calibrating TSC (%d second window) ... ", kTSCCalibrationSeconds);
	kCPUCyclesPerSecond = tscGetCyclesPerSecond((uint32_t)kTSCCalibrationSeconds);
	printf("%lu cycles/sec\n", kCPUCyclesPerSecond);

	printf("Detected cpu: %s\n", &kcpuInfo.brand_name);

	// USB last among the bus drivers: the P5 has no PS/2 port, so this is
	// where its keyboard and mouse come from. Runs BEFORE task creation on purpose —
	// the xHCI MMIO mapping lands in the kernel PML4's upper half and every
	// task PML4 clones those entries at birth (see xhci.c).
	if (kEnableUSB)
	{
		printf("Initializing USB (xHCI): ");
		init_xHCI();
	}

	// NIC bring-up rides the same rules as xHCI: before task creation, so
	// the MMIO mappings land in the kernel PML4's upper half and every
	// task PML4 clones them at birth. Quietly a no-op when no NIC is
	// attached — a netless boot is a configuration, not an error.
	// STACK BEFORE DRIVER, deliberately: init_net_stack claims the seam's
	// RX handler (and parses IP=/GW=/MASK=) before any NIC exists, so
	// there is no boot window where a frame can arrive unclaimed — the
	// rx_dropped_no_handler counter should only ever move in a build
	// where someone unhooked the stack on purpose.
	if (kEnableNet)
	{
		init_net_stack();
		init_virtio_net();
		// The e1000 AFTER virtio, deliberately: registration order is
		// device order, and kNetDevices[0] is the NIC the stack dials
		// through. A machine offered both keeps the driver that has flown
		// the most miles; a machine offered only an e1000 (VirtualBox's
		// default adapter, `-device e1000` on QEMU) gets it as device 0
		// and never notices the difference. That last clause is the whole
		// point of the seam.
		init_e1000();
		// And the RTL8125, last for the same reason the e1000 came second:
		// registration order is device order and the stack dials through
		// kNetDevices[0]. On the P5 — the only machine in this house that
		// HAS one — it will be the sole NIC and therefore device 0 anyway.
		// Everywhere else this call finds nothing and returns, which is what
		// keeps every existing QEMU boot byte-for-byte as green as before it
		// existed. (It registers nothing with the seam until it can actually
		// transmit — see the closing comment in r8125_init_device.)
		init_r8125();
		// DHCP by default when a NIC exists and nobody typed IP= — the
		// lease overwrites the static convention defaults when it lands
		// (and if no server answers, those defaults keep working; the
		// whole policy is argued in dhcp.h). The opening DISCOVER goes
		// out right here; replies and retries ride the processSignals
		// poll once the scheduler is up.
		if (kNetDeviceCount > 0 && kNetIPString[0] == '\0')
			dhcp_start(kNetDevices[0]);
	}
    printf("SMP: Initializing ... ");
    kLimineSMPInfo = smp_request.response;
    init_SMP(kEnableSMP);
    printf("(%u core(s) initialized)\n", kMPCoreCount);
    // Say which scheduler this boot got — the mode is a cmdline decision now
    // (tickless default, SCHED=periodic opt-out), and a decision that changes
    // how every core behaves should be readable on the glass, not inferred.
    // The banner tells the whole scheduling truth for this boot: mode, and —
    // under tickless — the lease length actually in force (BACKSTOP=<ms>
    // overrides the default, so the number must come from the variable, not
    // the constant; a human comparing GUI smoothness across boots reads it
    // here instead of guessing which cmdline won).
    if (!kTicklessScheduler)
        printf("Scheduler: periodic (legacy 100Hz all-core; diagnostic mode)\n");
    else if (kSchedBackstopEnabled)
        printf("Scheduler: tickless (park-and-nudge; %dms backstop lease; SCHED=periodic for legacy)\n",
               kSchedBackstopMS);
    else
        printf("Scheduler: tickless (park-and-nudge; NOBACKSTOP — hogs are immortal; SCHED=periodic for legacy)\n");

    init_signals();

	create_kernel_task();

	ap_initialization_handler();

	// Route the PIT through the IOAPIC. The vector is named rather than
	// hard-coded because moving it is a live question, not a settled one — the
	// constant carries the whole story of the 2026-08-10 attempt and why it
	// came back (see IRQ0_APIC_VECTOR in smp_core.h).
	remap_irq0_to_apic(IRQ0_APIC_VECTOR);

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

	// The e1000's INTx doorbell rides the same platform moment: the rings
	// came up back in init_e1000 (long before APIC mode existed), but a PCI
	// interrupt needs the IOAPIC, so adoption waits until here — rings at
	// driver init, doorbell at platform init, the keyboard's precedent one
	// paragraph up. Routing is discovered by PROBE (the card can ring its
	// own bell — see e1000_enable_intx), so this works on q35, PIIX3, or
	// bare metal without an AML interpreter. No NIC, no IOAPIC, or a silent
	// probe all degrade to the polled path — never a dead network.
	e1000_enable_intx();

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
        printf("Running pre-boot tests ...\n");
        test_run_preboot();   // prints its own "pre-boot tests: N passed, M failed"
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
			// Hand each core's CLS its idle THREAD — the target the
			// accounting charge sites redirect to while acctCurrentHalted
			// is up (smp.h has the doctrine). Set here, once, before the
			// scheduler ever runs a pass.
			get_core_local_storage_for_core(cnt)->acctIdleThread =
				kIdleTasks[cnt]->threads;
		}

	BOOTMARK("idle-tasks-created");

	#if ENABLE_LOG_BUFFERING == 1
	    kLogDTask = task_create("/logd", 0, NULL, kKernelTask, true, THREAD_NO_AFFINITY);
	    // Pass daemon=true (first arg in RDI) to logd_thread
	    kLogDTask->threads->regs.RDI = 1;
	    scheduler_submit_new_task(kLogDTask);
	#endif
	BOOTMARK("logd+kworker-created");

	// THE UNDERTAKER RUNS ANYWHERE (2026-08-09, Chris's ruling: "no reason for
	// it to be pinned at all"). It used to be nailed to kCPUInfo[1], and that
	// pin turned a runaway user program into an OS-WIDE outage: under tickless
	// an AP does not preempt, so any syscall-free task landing on core 1 owned
	// that core forever, kworker went RUNNABLE-and-never-running, and since it
	// is the only undertaker NOTHING anywhere got buried — every task that
	// exited on every terminal became a permanent zombie until reboot. Caught
	// by `watch` piling up corpses; reproduced in QEMU with kworker reading
	// 0:00.0 CPU on a core a spinner held. Unpinned, the damage is bounded to
	// the one core the hog stole. (That is a MITIGATION, not the cure: the
	// cure is an AP preemption backstop so no core can be owned forever —
	// see the tickless note in scheduler.c's nudge loop.)
	//
	// The kMPCoreCount > 1 guard went with it, and it was load-bearing in the
	// worst way: it existed ONLY because core 1 had to exist to be pinned to,
	// which meant a `nosmp` or single-core boot got NO kworker — and therefore
	// no burial at all, permanently, on exactly the machines least able to
	// spare the memory. logd (just above) has always been NO_AFFINITY; the
	// undertaker is a daemon like any other and is now spelled that way.
	if (kEnableKWorker)
	{
	    kKWorkerTask = task_create("/kworker", 0, NULL, kKernelTask, true, THREAD_NO_AFFINITY);
	    kKWorkerTask->threads->regs.RDI = 1;
	    kKWorkerTask->autoReap = true;
	    printd(DEBUG_TASK | DEBUG_DETAILED,
	    	"kernel_init: enabling /kworker (unpinned; task=0x%08x, thread=0x%08x)\n",
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

	// /sys — the machine as files (sysfs.c has the doctrine): PCI discovery's
	// results, readable at /sys/bus/pci. Same synthetic-mount rules as /proc
	// (no GUID, no disk, mounted after the sweep for the same reason).
	sysfs_mount();

	// /dev — the kernel's objects as files (devfs.c has the doctrine): null,
	// zero, full, and tty. Same synthetic-mount rules as /proc and /sys (no
	// GUID, no disk, mounted after the sweep for the same reason), and the
	// third leg of the promise in SUCCESSION.md's curated-tree list.
	devfs_mount();

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
		// argv[2], when present, is the format name LOGFMT= gave SERIAL. It is
		// the bottom rung of the file's format gradient (logd's own comment
		// carries the whole ladder): a config file outranks it, but with no
		// config file at all, one LOGFMT= sets both sinks — which is what
		// somebody who only wants one knob means by turning that knob.
		char *logdArgv[] = { "/bin/logd", kLogdPath, kLogFormatName };
		int logdArgc = (kLogFormatName[0] != '\0' && !kLogFormatBad) ? 3 : 2;
		task_t *logdTask = task_create("/bin/logd", logdArgc, logdArgv, kKernelTask, false, THREAD_NO_AFFINITY);
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

	// The standing NMI-probe diagnostic (NMIPROBE — see kTestNmiProbe's comment
	// in kernel_commandline.c). Runs BEFORE the panic test below, because a
	// panic ends the boot and this must actually get to run. Every core should
	// answer and resume; the machine carrying on normally afterwards is half of
	// what this proves.
	if (kTestNmiProbe)
		nmi_probe_sweep();

	// WATCHDMA's arming point: after every boot-time mapping is done, so the
	// watchpoints see only writes that boot did not make. (nvme.c explains why
	// this cannot happen at controller init.)
	nvme_watch_dma_chain();

	// The standing WATCHPOINT diagnostic (TESTWATCH — watchpoint.h). Newest
	// member of the TESTPANIC family, and it earns its place for the family's
	// usual reason: a watchpoint only ever runs when something has already
	// gone wrong, so without a standing test it would rot silently and be
	// broken on the one night it mattered.
	//
	// It proves the WHOLE chain in one boot: DR7 programming, the trap
	// reaching the unified #DB path, the hit being attributed to the right
	// slot, the report naming the watchpoint, and — the part that is easy to
	// get wrong and impossible to notice — TRACE mode actually RESUMING. Two
	// hits are expected: one trace (the machine keeps going) and then a halt.
	if (kTestWatchpoint)
	{
		static volatile uint64_t watchBait = 0;

		printf("TESTWATCH: arming a trace watchpoint on the bait at 0x%016lx\n",
		       (uintptr_t)&watchBait);
		int slot = watchpoint_arm((uintptr_t)&watchBait, 8, WATCH_WRITE, WATCH_TRACE,
		                          "TESTWATCH bait (trace)");
		if (slot >= 0) {
			// Should report and CONTINUE — if the machine stops here, resume
			// is broken, which is the single most important thing this proves.
			watchBait = 0x5741544348454431ULL;   // "WATCHED1"
			printf("TESTWATCH: survived the trace hit — resume works\n");
			watchpoint_disarm(slot);

			// Now the same store under a HALT watchpoint: the machine should
			// stop here with a full report and never reach the line below.
			slot = watchpoint_arm((uintptr_t)&watchBait, 8, WATCH_WRITE, WATCH_HALT,
			                      "TESTWATCH bait (halt)");
			if (slot >= 0) {
				watchBait = 0x5741544348454432ULL;   // "WATCHED2"
				printf("TESTWATCH: *** STILL RUNNING after a HALT watchpoint — "
				       "the watchpoint did NOT fire ***\n");
			}
		}
	}

	// The standing fatal-page-fault diagnostic (TESTPF). A wild KERNEL pointer,
	// which is the exact shape of the fault this report exists to explain — an
	// upper-half address no VMA covers, touched from ring 0. The report that
	// comes out should be indistinguishable in structure from a #GP's: banner,
	// AP/thread, faulting RIP, error code, CR2/CR3, interrupted RSP, the full
	// register set, and a call chain.
	if (kTestPageFault)
	{
		// volatile so the compiler cannot decide a store nobody reads is
		// removable — the whole point is that the store actually executes.
		volatile uint64_t *wild = (volatile uint64_t *)0xffff8f00deadb000ULL;
		*wild = 0x5157434544414544ULL;
	}

	// The standing #GP diagnostic (TESTGP — see kTestGPFault's comment in
	// kernel_commandline.c). A NON-CANONICAL dereference, which is a #GP, not
	// a #PF — the CPU rejects the address before paging ever sees it. The
	// signatures are the test: under the old stubs a #GP printed the LAST PAGE
	// FAULT's registers, so a report showing RAX=0xdead00000000beef and
	// R15=0x4750323032363038 ("GP202608" — this test's mark) proves the
	// capture belongs to THIS exception. Registers loaded in the same asm
	// block as the fault so no compiler decision can separate them.
	if (kTestGPFault)
	{
		__asm__ volatile(
			"mov r15, 0x4750323032363038\n\t"   // "GP202608" — the signature
			"mov rax, 0xdead00000000beef\n\t"    // non-canonical on purpose
			"mov [rax], r15\n\t"
			::: "rax", "r15", "memory");
	}

	// The standing panic-pipeline diagnostic (see kTestPanic's comment in
	// kernel_commandline.c). Placed HERE, after the full boot + test flow, so
	// the log queues carry a realistic backlog for the emergency flush to
	// prove itself against.
	if (kTestPanic)
		panic("TESTPANIC: deliberate panic — if you can read this in the serial log, the panic pipeline works\n");

	// The descent's own diagnostic (SHUTDOWNTEST, 2026-08-08 — same pattern
	// as TESTPANIC): run the full shutdown after boot + tests, with logd
	// live and files freshly written, so every step has real work to do.
	// Under QEMU the poweroff port makes the process EXIT — a scriptable
	// pass/fail for the whole pipeline.
	if (kTestShutdown)
		shutdown_system();

    // Launch the shells and park the kernel task so the scheduler keeps
    // running it (husk loops forever on read/spawn/wait). The kernel task is
    // husk's parent, so husk inherits its environment. (When husk becomes the
    // real init path this replaces the HELLO/KEYTEST temps entirely.)
    //
    // TWO shells at boot — the os32 loadout, kept on purpose (eight
    // terminals, TERMINAL_SHELL_COUNT=2): VT1 shares the system console with
    // the kernel's own chatter, VT2 is all yours, and VT3-8 wait dark with a
    // summons on the door (first keystroke raises a shell there — V6 read
    // /etc/ttys at boot and hung a shell on every line; os64 hangs one the
    // moment you knock). tty_seat_shell is the whole ceremony: Ctrl+C
    // immunity (controllingShell — an ETX with the shell foreground stays a
    // DATA byte, husk line-kills), the terminal of record, and foreground —
    // all set BEFORE the task can run. (SIGINT.md)
    if (kRunHusk && kRootFilesystem != NULL)
    {
        printf("Launching /bin/husk (the shell) ...\n");
        // autoReap: COLLECTED BY DECREE, because ktask never waits (it has no
        // wait loop and never will — see task_reparent_orphans' decree for the
        // same reasoning on init's orphans). Without this, exiting a boot
        // shell leaves an IMMORTAL ZOMBIE: retValCollected never (no waiter),
        // autoReap false, parent alive forever — the corpse fails the
        // undertaker's collected test on every sweep until reboot. Found
        // 2026-08-15 on the P5 as "15 zombies kworker won't touch," ~1.1MB of
        // stacks each; Chris proved the mechanism live (a NESTED husk exits
        // clean — its parent waits; only ktask's children stuck), and the
        // guest-side graveyard census matched. The testrun launch below had
        // carried this exact flag, with a comment PREDICTING this exact bug,
        // since 2026-08-10. The shell's exit status goes to no one; that is
        // the licence.
        task_t *huskTask = task_create("/bin/husk", 0, NULL, kKernelTask, false, THREAD_NO_AFFINITY);
        if (huskTask)
        {
            huskTask->autoReap = true;
            tty_seat_shell(&kTTY[0], huskTask);
            scheduler_submit_new_task(huskTask);
        }
        else
            printf("  /bin/husk launch failed (not on the image?)\n");

        task_t *huskTask2 = task_create("/bin/husk", 0, NULL, kKernelTask, false, THREAD_NO_AFFINITY);
        if (huskTask2)
        {
            huskTask2->autoReap = true;   // same decree as VT1's shell above
            tty_seat_shell(&kTTY[1], huskTask2);
            scheduler_submit_new_task(huskTask2);
        }
        // No lament if the second seat fails: a blank dormant VT2 still
        // answers a knock like any other terminal.
    }
    
    // The ring-3 half of the suite (TESTRUN, 2026-08-10). It lives out here and
    // not in test_run_postboot() for the reason it was extracted in the first
    // place: these fixtures test whether a PROGRAM runs, exits, and hands back
    // the right code, and the only honest way to ask that is from a program.
    // It has to come AFTER the husk block — testrun's own output goes to the
    // console, and the terminals are seated up there.
    //
    // autoReap is set deliberately, and it is the OPPOSITE of the rule the
    // in-kernel fixtures follow: those poll `exited`/`retVal` off the corpse,
    // so kworker must not bury it out from under them (test_main.c's
    // test_spawn/test_release, 2026-08-09). Nothing here reads testrun's
    // corpse — the verdict comes back on the wire from testrun itself — so
    // handing kworker the licence immediately is right, and skipping it would
    // leave a permanent zombie that `ps` would report forever.
    if (kRunTestrun && kRootFilesystem != NULL)
    {
        printf("Launching /bin/testrun (ring-3 test suite) ...\n");
        task_t *testrunTask = task_create("/bin/testrun", 0, NULL, kKernelTask, false, THREAD_NO_AFFINITY);
        if (testrunTask)
        {
            testrunTask->autoReap = true;
            scheduler_submit_new_task(testrunTask);
        }
        else
            printf("  /bin/testrun launch failed (not on the image?)\n");
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
		// Park quietly. This loop used to printf a status line onto the
		// glass every pass — bring-up scaffolding, "living proof the
		// print_n diversion works end to end" — and it served exactly until
		// the day a real shell moved into the console window (2026-08-17,
		// the first `ls` at a husk> prompt inside the GUI), at which point
		// proof-of-life became somebody typing over your prompt every five
		// seconds. The heartbeat survives on the SERIAL wire under
		// DEBUG_GUI, where a wedged-kernel-task diagnosis actually goes
		// looking for it; the glass belongs to the user now.
		uint64_t statusCount = 0;
		while (1)
		{
			sigaction(SIGSLEEP, NULL, kTicksSinceStart + 5 * TICKS_PER_SECOND, kKernelTask->threads);
			printd(DEBUG_GUI, "os64: up %lu ticks, GUI running (status #%lu)\n", kTicksSinceStart, ++statusCount);
		}
	}

	kernel_park();
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

	// Adopt LOGFMT= IMMEDIATELY — before hardware_init, before the banner,
	// before anything has been logged at all — so the whole boot renders in
	// one format instead of switching partway down. Nothing can be said about
	// a bad name yet (no serial, no framebuffer), so the verdict is parked in
	// kLogFormatBad and reported a few lines below, once there is somewhere
	// to report it to.
	kLogFormatBad = (kLogFormatName[0] != '\0' && !log_set_format_by_name(kLogFormatName));
	hardware_init();
	strftime_epoch(&startTime[0], 100, "%m/%d/%Y %H:%M:%S", kSystemCurrentTime + (kTimeZone * 60 * 60));
	// A SERIAL= value that is neither 'on' nor 'off' (values are
	// case-sensitive, like every token this cmdline parses) must not be
	// swallowed: this is the one flag whose silent failure costs the wire
	// itself. Same pattern as kLogFormatBad above — the verdict is parked
	// here and reported beside the serial banner below, once the glass
	// exists to carry it. (Declared outside the ENABLE_COM1 guard because
	// the banner that reads it prints unconditionally.)
	bool serialOverrideBad = false;
#ifdef ENABLE_COM1
	// KEEP THE ANSWER. This return value was discarded for the whole life of
	// the project, and the probe behind it never actually tested anything
	// (fixed 2026-08-18) — so the kernel could not tell a machine with a
	// serial port from one without, and cheerfully "drained" the log into a
	// UART that wasn't there. SERIAL=on|off overrules the probe either way.
	bool serialProbed = (init_serial(0x3f8) == 0);
	if (strcmp(kSerialOverride, "on") == 0)
		kSerialPresent = true;
	else if (strcmp(kSerialOverride, "off") == 0)
		kSerialPresent = false;
	else
	{
		kSerialPresent = serialProbed;
		serialOverrideBad = (kSerialOverride[0] != '\0');
	}
#endif
	kKernelPML4v = kHHDMOffset + kKernelPML4;

 	init_video(framebuffer_request.response->framebuffers[0], limine_module_response);
	// The build stamp rides the banner so the FIRST line on the glass names
	// which kernel this is — the receipt for a kernel that arrived over the
	// wire (os64get -> /fat/boot -> reboot), and the instant answer to "did
	// my new build actually boot?" ever after.
	printd(DEBUG_BOOT, "***** OS64 (built %s) - system booting at %s *****\n", kBuildStamp, startTime);
	printf(	"***** OS64 (built %s) - system booting at %s *****\n", kBuildStamp, startTime);
	uint64_t high, low;
	parse_debug_level(kDebugLevel, &high, &low);
	// Say the verdict on the GLASS. On a machine with no UART this is the only
	// place it can be said, and it is the sentence that explains why serial is
	// quiet and where the log went instead.
	// "(forced by SERIAL=)" only when the override actually took: a garbage
	// value falls back to the probe, and claiming it was forced would be the
	// banner lying about who decided.
	printf("Serial port (COM1): %s%s\n",
	       kSerialPresent ? "present" : "ABSENT — kernel log retained in memory, not drained",
	       (kSerialOverride[0] && !serialOverrideBad) ? " (forced by SERIAL=)" : "");
	if (serialOverrideBad)
	{
		printf("SERIAL=%s is neither 'on' nor 'off' (case matters) — trusting the probe\n",
		       kSerialOverride);
		printd(DEBUG_BOOT, "SERIAL=%s is neither 'on' nor 'off' (case matters) — trusting the probe\n",
		       kSerialOverride);
	}
	printf("Commandline: %s (debug level 0x%016lx%016lx)\n",kKernelCommandline, high, low);
    // The '\n' is not decoration: without it the next entry appends to this
    // line's tail, which is why every serial log for years read
    // "...LOGD=/home/os64.log2 (0x0000) AP0: DEBUG_OPTIONS ...". Caught while
    // the log format was under the microscope, 2026-08-18.
    printd(DEBUG_BOOT, "Commandline: %s\n", kKernelCommandline);

	// The COMPLAINT about a bad LOGFMT= waits until here, where serial and the
	// framebuffer both exist to carry it — but the format itself was adopted
	// far earlier (right after the commandline was parsed), so the log is one
	// format from its first line rather than switching a few lines in. An
	// unknown name keeps `classic` and says so on both the glass and the wire:
	// a format is the instrument you read every other failure through, so a
	// silent fallback would be the one lie that hides all the others.
	if (kLogFormatBad)
	{
		printf("LOGFMT=%s is not a known format (classic, daily, full) — keeping classic\n",
		       kLogFormatName);
		printd(DEBUG_BOOT, "LOGFMT=%s is not a known format (classic, daily, full) — keeping classic\n",
		       kLogFormatName);
	}
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

	// The log rings, as early as they can possibly exist (moved here from
	// kernel_init 2026-08-18). They need exactly two things — kmalloc, which
	// the line above just proved, and kLimineSMPInfo->cpu_count, which
	// main.c set before kernel_main was ever called — and every printd
	// BEFORE this point takes printd's `else` branch: formatted straight
	// onto COM1 and stored nowhere, so it can never reach logd's file.
	//
	// That cost the whole boot story. Measured 2026-08-18 on an ext2 LOGD=
	// boot: the file's earliest entry was tick 129, while the banner,
	// commandline, symbols, ACPI, TSS, SMP bring-up and PCI enumeration
	// existed ONLY on the serial log — invisible on a machine like the P5
	// that has no serial port at all. Sitting down here bought nothing:
	// ACPI and the GDT do not care whether logging is queued, and a boot
	// that dies before the scheduler still has DIRECTLOG (kDirectLog) to
	// bypass the queues entirely.
	//
	// Everything above this line is still serial-only, and that residue is
	// now four lines — the banner, the commandline, and the debug level —
	// because they print before the allocator exists.
	logging_queueing_init();

	// kmalloc works now — give the console its RAM shadow so scrolling stops
	// READING VRAM (fine in QEMU's RAM framebuffer, ~2 lines/second on the
	// P5's real write-combined VRAM; see scroll_framebuffer_full).
	renderer_attach_shadow();
	// And immediately raise the virtual terminals on top of it (tty.h for
	// the doctrine): from this line on, every printf lands in VT1's grid —
	// the system console — and the glass is a projection of whichever
	// terminal is focused. Done HERE, right after the shadow, so the whole
	// of kernel_init's boot spew is in the grid and survives an Alt+F2 away
	// and back; the handful of lines above exist only as pixels.
	tty_init();
	printf("Virtual terminals: %u (Alt+F1..F%u or Alt+Arrows to switch; Shift+PgUp/PgDn for scrollback)\n",
	       TTY_COUNT, TTY_COUNT);
	kernel_init();
}
