#include "CONFIG.h"
#include "serial_logging.h"
#include "allocator.h"
#include "memory/paging.h"   // paging_pool_pages_used — the park loop's pool probe
#include "BasicRenderer.h"
#include "strings/sprintf.h"
#include "strftime.h"
#include "signals.h"
#include "task.h"
#include "io.h"
#include "shutdown.h"
#include "log.h"          // the klog retire handshake (kernel side)
#include "vfs.h"          // vfs_sync_all — the open-file registry sweep
#include "memory/vma.h"   // call_in_kernel_context — file I/O runs in kernel context
#include "scheduler.h"    // scheduler_trigger — yielding while logd drains
#include "smp_core.h"     // get_core_local_storage — scheduler_trigger wants the CLS
#include "driver/system/nvme.h"   // nvme_flush_all — the drive's volatile cache
#include "driver/system/acpi.h"   // acpi_poweroff — the real soft-off, S5

int usedCount=0;
extern volatile uint64_t kSystemCurrentTime;
extern volatile uint64_t kTicksSinceStart;
extern BasicRenderer kRenderer;
extern volatile uint64_t kSystemStartTime;
extern task_t* kKernelTask;

// (Named shutdown() from the day this file was born — see shutdown.h for
// why the name finally moved to the function that earns it.)
void kernel_park(void)
{
	uint64_t memInUse=0;
    uint64_t lastTime = 0;
    char heartbeat[2] = "*";
    printd(DEBUG_BOOT | DEBUG_DETAILED, "BOOT END: Status of memory status (%u entries):\n",kMemoryStatusCurrentPtr);
	for (uint64_t cnt=0;cnt<kMemoryStatusCurrentPtr;cnt++)
	{
        printd(DEBUG_BOOT | DEBUG_DETAILED, "\tMemory at 0x%016Lx for 0x%016Lx (%Lu) bytes is %s\n", kMemoryStatus[cnt].startAddress, kMemoryStatus[cnt].length, kMemoryStatus[cnt].length, kMemoryStatus[cnt].in_use ? "in use" : "not in use");
        if (kMemoryStatus[cnt].in_use)
		{
			usedCount++;
			memInUse+=kMemoryStatus[cnt].length;
		}
	}
    printd(DEBUG_BOOT | DEBUG_DETAILED, "Found %u memory in use at shutdown\n", memInUse);
    printd(DEBUG_BOOT | DEBUG_DETAILED, "Found %u memory status entries,  %u in use\n", kMemoryStatusCurrentPtr, usedCount);
    // This used to say "Idling for now until we have a userland to run!" — and
	// as of husk, that is no longer true. The kernel task doesn't idle here
	// waiting for a userland; it parks here BECAUSE the userland is running.
	// The scheduler keeps husk (and whatever husk spawns) on the CPUs while
	// this loop naps and repaints the clock. os64 is a system that stays up.
	printf("Kernel loaded! Userland is running — the kernel task parks here.\n");

	// Boot-complete pool receipt (probe, 2026-08-04, the 640-page exhaustion
	// hunt): how much of the paging pool did BOOT consume? Straight to the
	// serial wire — panic's door — so it's readable live even when a logd
	// claim owns the log. Splits the mystery in two: everything after this
	// line is runtime consumption, and the park loop below reports its slope.
	{
		char poolMsg[128];
		sprintf(poolMsg, "[pool] boot complete: %lu/%lu paging pages used\n",
		        paging_pool_pages_used(), kPagingPagesCount);
		serial_print_string(poolMsg);
	}
	uint64_t lastPoolReport = kSystemCurrentTime;
	uint64_t lastPoolUsed = paging_pool_pages_used();

	while (true)
	{
		if (lastTime != kSystemCurrentTime)
		{
            if (kSystemCurrentTime % 2 == 0)
                print_at(&kRenderer, 115, 0, heartbeat);
            else
                print_at(&kRenderer, 115, 0, " ");
			lastTime = kSystemCurrentTime;

			// The heartbeat (used to be clock) is a STATUS WIDGET, not a console write. print_at()
			// paints it at a fixed cell without touching the console cursor —
			// so it cannot land in the middle of husk's prompt, and there is no
			// save/moveto/print/restore sequence to race against husk echoing
			// keystrokes on another core. (That race is exactly what the old
			// cursor save/restore had; it just hadn't bitten yet.)

			// Pool slope report, once a minute, serial-direct (same rationale
			// as the boot receipt above). The DELTA is the story: a healthy
			// steady state is +0/min; the address-march bug read as a steady
			// +30ish/min climb toward the exhaustion panic.
			if (kSystemCurrentTime - lastPoolReport >= 60)
			{
				uint64_t used = paging_pool_pages_used();
				char poolMsg[128];
				sprintf(poolMsg, "[pool] %lu/%lu paging pages used (+%lu this minute)\n",
				        used, kPagingPagesCount, used - lastPoolUsed);
				serial_print_string(poolMsg);
				lastPoolReport = kSystemCurrentTime;
				lastPoolUsed = used;
			}
        }
        signal_raise(SIGSLEEP, kTicksSinceStart+90,kKernelTask->threads);
	}
	while (true) {asm("sti\nhlt\n");}
}

// ── shutdown_system: the ordered descent (2026-08-08) ────────────────────────
// The order IS the design — each step quiets the writer the next step flushes
// behind:
//
//   1. announce            — the operator knows the machine heard them
//   2. retire logd         — the ONE continuous writer on a parked system;
//                            scheduler must still be alive here so the daemon
//                            can run its final drain (this is why AP/scheduler
//                            quiescing does NOT lead the list)
//   3. vfs_sync_all        — FAT's deferred lengths, anything still open
//   4. FLUSH CACHE (NVMe)  — the drive's volatile cache: the one tier no
//                            fs-level sync can reach, and the entire reason
//                            "sync then power button" was a ritual, not a
//                            guarantee
//   5. power off           — hypervisor exit ports; on hardware whose ACPI we
//                            don't speak yet (a real S5 needs the DSDT's \_S5
//                            out of an AML interpreter we haven't built), the
//                            1995 liturgy and a parked core — which, after
//                            steps 2-4, is a TRUE statement
//
// Not here, on purpose:
//   - ext2 s_state clean-mark: meaningful only with dirty-at-mount as its
//     other half, and THAT breaks the host fsck-green workflow until the
//     harness learns shutdown (every monitor-quit QEMU run would read dirty).
//     It belongs to the native-fsck arc — see its DEBTS row.
//   - Hard AP quiesce (IPI-halt): under the tickless default, APs with no
//     runnable thread are already hlt-parked, logd is retired by step 2, and
//     the foreground shell is blocked in wait() — the machine is quiescent in
//     practice. A belt-and-suspenders IPI-halt vector is a follow-on.
//
// Runs in the CALLING task's context (any CR3: everything it touches lives
// in the shared upper half; file I/O hops through call_in_kernel_context
// exactly like every other syscall body). Never returns.

// The sync result lives in a FILE-SCOPE static, never a caller local: the
// caller runs on the thread's syscall kernel stack — a TASK-LOCAL VA that
// kKernelPML4 does not map — so &local handed across call_in_kernel_context
// is an unmapped address the moment the continuation runs under kernel CR3.
// This function wrote through exactly that pointer on its first ring-3
// flight (2026-08-08, Chris's /bin/shutdown — "no VMA" panic mid-descent;
// the SHUTDOWNTEST flights flew clean only because the KERNEL task's stack
// is kKernelPML4-mapped). syscall_sync_all kmallocs its params for this
// same reason; shutdown gets the simpler cure because it runs once per
// power cycle by definition — kernel .bss is mapped under every CR3.
static int64_t sh_synced;

// See shutdown.h: the descent's "stand down" sign for the hangup sweep.
volatile bool kShuttingDown = false;

static void shutdown_sync_all_in_kernel_context(void *arg)
{
	(void)arg;
	sh_synced = vfs_sync_all();
}

// Same hop for the drive flush: NVMe I/O otherwise only ever runs under
// kernel CR3 (every file-I/O path goes through call_in_kernel_context), and
// the descent should not be the one caller that exercises it from a task
// CR3 for the first time at the worst possible moment.
static void shutdown_flush_in_kernel_context(void *arg)
{
	(void)arg;
	nvme_flush_all();
}

// ── The termination ladder (2026-08-21) ─────────────────────────────────────
// SIGTERM, wait, SIGKILL. Every init system that ever powered a machine down
// has this shape, for the same reason: a program asked to stop can close its
// files, finish its line, and leave a clean exit code, while a program shot in
// the head leaves whatever it left. The grace period is the entire negotiation.
//
// Who is exempt, and why each one:
//   - EVERY KERNEL TASK (`t->kernelTask`), which is the important one and the
//     one the first draft got wrong. The first flight asked NINETEEN tasks to
//     stop and had to kill twelve — because kTaskList is not a list of
//     programs, it is a list of TASKS: the per-core IDLE tasks are on it, so
//     is kworker, so is the compositor. SIGKILLing the idle task of a core
//     leaves that core's scheduler with nothing to run at the exact moment
//     the descent still needs CPU to sync and flush. A shutdown that has to
//     shoot the machine's own scaffolding to proceed is not an orderly one.
//     Ring 3 is the whole audience here: a kernel task IS the machine, and
//     the machine stops when the power does.
//   - the CALLER: it is running this descent; it dies at the poweroff.
//   - the LOG DAEMON: it has its own retirement handshake two steps below,
//     and it must outlive this sweep so the exits it is about to witness
//     actually reach the log. Identified by its SINK CLAIM, never by name —
//     kLogSinkOwnerTask is the fact, "logd" is a spelling.
//
// The order matters: the ladder runs BEFORE logd retires, so every "task X
// terminating (exit 143)" line lands in the file. That is the whole reason
// this is step 2 and not step 4.
// The dial lives in CONFIG.h, in milliseconds; ticks are this file's problem.
#define SHUTDOWN_GRACE_TICKS ((SHUTDOWN_GRACE_MS) / (MS_PER_TICK))

// ── waiting, when the clock might be dead ───────────────────────────────────
//
// Every wait in this descent is a wait on kTicksSinceStart, and the P5 taught
// us (2026-08-21) that a descent can run on a core where that number does not
// advance. One capped loop is not a fix, it is a fix for ONE loop: the first
// draft capped the termination grace and left the log-retirement wait and its
// grace bare, so a stalled clock still hung the machine — thirty lines further
// down, having just printed "proceeding anyway" (Codex review, 2026-08-22).
//
// So the seatbelt lives here, once, and every wait wears it:
//   - the DEADLINE is the policy — how long this step is worth waiting.
//   - the SPIN CAP is the seatbelt. Enormous next to the real wait (millions
//     of yields against the couple of hundred a 2-second grace takes), so it
//     can only ever fire when the clock is genuinely not moving.
//   - and the stall is REMEMBERED. Once we have caught the clock standing
//     still, no later step gets to discover it the slow way: the budget
//     becomes a fixed number of yields, enough to let a ring-3 daemon finish
//     what it was doing, and the descent keeps walking.
#define SHUTDOWN_MAX_SPINS      20000000ULL
#define SHUTDOWN_STALLED_YIELDS 100000ULL

static bool sh_clockStalled = false;   // sticky for the whole descent

// Yield for `ticks`, or until `stillWaiting()` says we are done (NULL = wait
// out the whole thing). Returns false if the seatbelt tripped.
//
// NO CACHED CLS. Every yield here can put this thread back on a DIFFERENT
// core, and a cached core_local_storage_t then names the core we used to be
// on: the trigger would nudge a stranger's APIC and leave the core we are
// actually running on unyielded, burning the budget without giving logd or a
// dying task a single extra pass. scheduler_trigger(NULL) re-fetches the CLS
// through GS on each call, which is exactly the case it re-fetches for.
// (Codex review, 2026-08-22 — the bug predates this helper: the loops it
// replaced passed a cls captured before their first yield too.)
static bool shutdown_wait(uint64_t ticks, bool (*stillWaiting)(void))
{
	uint64_t deadline = kTicksSinceStart + ticks;
	uint64_t spins    = 0;
	uint64_t maxSpins = sh_clockStalled ? SHUTDOWN_STALLED_YIELDS : SHUTDOWN_MAX_SPINS;

	while (stillWaiting == NULL || stillWaiting())
	{
		if (!sh_clockStalled && kTicksSinceStart >= deadline)
			return true;
		if (++spins > maxSpins)
		{
			sh_clockStalled = true;
			return false;
		}
		scheduler_trigger(NULL);
	}
	return true;
}

static bool shutdown_task_is_exempt(const task_t *t, const task_t *self)
{
	if (t == NULL || t == self || t == kKernelTask)
		return true;
	if (t->kernelTask)
		return true;      // idle tasks, kworker, the compositor — scaffolding
	if (t->exited)
		return true;
	return (kLogSinkOwnerTask != 0 && t->taskID == kLogSinkOwnerTask);
}

static void shutdown_terminate_tasks(void)
{
	core_local_storage_t *cls = get_core_local_storage();
	task_t *self = cls ? cls->task : NULL;
	uint32_t asked = 0;

	for (task_t *t = kTaskList; t != NULL && t != (task_t *)NO_TASK; t = t->next)
	{
		if (shutdown_task_is_exempt(t, self))
			continue;
		task_signal_and_nudge(t, SIGTERM);
		asked++;
		printd(DEBUG_SHUTDOWN, "[shutdown] SIGTERM -> %s (task %lu)\n",
		       t->exename, t->taskID);
	}

	if (asked == 0)
	{
		printf("  no tasks to stop\n");
		printd(DEBUG_SHUTDOWN, "  no tasks to stop\n");
		return;
	}
	printf("  asked %u task%s to stop\n", asked, asked == 1 ? "" : "s");
	printd(DEBUG_SHUTDOWN, "  asked %u task%s to stop\n", asked, asked == 1 ? "" : "s");

	// Yield while they go. Ending EARLY when the last one is gone is the
	// point of watching rather than sleeping — a quiet machine should not
	// wait out a timeout it has already satisfied. That per-pass headcount is
	// why this wait is spelled out here instead of calling shutdown_wait();
	// the two limits it wears are the same ones, from the same constants.
	//
	// This loop is where the P5 froze — reliably enough to notice, randomly
	// enough to be maddening: "asked 4 tasks to stop" and then nothing, ever
	// (Chris, 2026-08-21). Whatever stalls kTicksSinceStart there (a descent
	// running where the timer that advances it does not) is worth finding on
	// its own, but the descent must not be the thing that hangs while we look
	// — a machine that will not power off is worse than one that powers off
	// impolitely. The cap is enormous compared to the real wait (millions of
	// yields against the couple of hundred a 2-second grace takes), so it can
	// only ever fire when the clock is genuinely not moving.
	uint64_t started  = kTicksSinceStart;
	uint64_t deadline = started + SHUTDOWN_GRACE_TICKS;
	uint64_t spins    = 0;
	uint32_t alive = 0;
	bool clockStalled = false;

	while (kTicksSinceStart < deadline)
	{
		alive = 0;
		for (task_t *t = kTaskList; t != NULL && t != (task_t *)NO_TASK; t = t->next)
			if (!shutdown_task_is_exempt(t, self))
				alive++;
		if (alive == 0)
			break;
		if (++spins > SHUTDOWN_MAX_SPINS)
		{
			// Sticky, not local: every wait BELOW this one is on the same
			// clock, and a clock that stalled here is still stalled there.
			clockStalled   = true;
			sh_clockStalled = true;
			break;
		}
		scheduler_trigger(NULL);   // NULL: this thread may wake on another core
	}

	if (clockStalled)
	{
		printf("  (the tick clock did not advance during the grace — "
		       "proceeding anyway, on a yield budget)\n");
		printd(DEBUG_SHUTDOWN, "  (the tick clock did not advance during the grace — "
		       "proceeding anyway, on a yield budget)\n");
	}

	// The elapsed time is REPORTED, not assumed. The grace is a ceiling of
	// SHUTDOWN_GRACE_MS, and how much of it actually gets spent is a fact
	// about the machine — one worth seeing rather than inferring, because
	// "that felt longer than two seconds" is not something anybody should
	// have to settle by stopwatch (his question, 2026-08-21).
	uint64_t spentMs = (kTicksSinceStart - started) * MS_PER_TICK;

	if (alive == 0)
	{
		printf("  all stopped cleanly (%lu ms)\n", (unsigned long)spentMs);
		printd(DEBUG_SHUTDOWN, "  all stopped cleanly (%lu ms)\n", (unsigned long)spentMs);
        return;
    }

	// The grace ran out. SIGKILL is not a request, and the survivors are
	// named on the wire — a program that had to be killed at shutdown is
	// worth knowing about tomorrow.
	uint32_t killed = 0;
	for (task_t *t = kTaskList; t != NULL && t != (task_t *)NO_TASK; t = t->next)
	{
		if (shutdown_task_is_exempt(t, self))
			continue;
		printd(DEBUG_SHUTDOWN, "[shutdown] SIGKILL -> %s (task %lu): "
		       "did not stop within %u ticks\n",
		       t->exename, t->taskID, SHUTDOWN_GRACE_TICKS);
		task_signal_and_nudge(t, SIGKILL);
		killed++;
	}
	printf("  %u task%s did not stop within %lu ms; killed\n",
	       killed, killed == 1 ? "" : "s", (unsigned long)spentMs);
    printd(DEBUG_SHUTDOWN, "  %u task%s did not stop within %lu ms; killed\n",
           killed, killed == 1 ? "" : "s", (unsigned long)spentMs);
    // A short second grace so the kills actually land before the disks go.
	// SAME SEATBELT as the grace loop above, for the same reason: this waits
	// on the same clock, and a clock that stalled fifteen lines ago is still
	// stalled here. (The first draft capped the grace wait and left this one
	// bare — found in review, 2026-08-22.)
	shutdown_wait(TICKS_PER_SECOND / 2, NULL);
}

void shutdown_system(os64_shutdown_mode_t mode)
{
	// 0. Raise the sign BEFORE anything is signalled. From here on, a shell
	//    exiting is a consequence of this descent and must not start a hangup
	//    cascade — one of whose victims would be this very task (shutdown.h).
	kShuttingDown = true;

	// 1. Announce — glass and wire both (the wire directly: logd is about to
	//    be retired, and this line must survive even if the descent wedges).
	printf("\nThe system is going down NOW!\n");
	serial_print_string(mode == OS64_SHUTDOWN_REBOOT
	                    ? "[shutdown] descent started (reboot)\n"
	                    : "[shutdown] descent started\n");

	// DEBUG_BOOT on purpose: the Ctrl+~ suppression leaves DEBUG_BOOT and
	// DEBUG_EXCEPTIONS on, so this line still reaches a log somebody has
	// quieted. (`nolog` zeroes the level outright and takes it too.)
	printd(DEBUG_BOOT, "The system is going down NOW!\n");
	// 2. Ask every program to stop, then insist. Before logd retires, so the
	//    exits make the log.
	shutdown_terminate_tasks();

	// 2. Retire the log daemon. Set the flag, then keep yielding so the
	//    daemon (a ring-3 task) gets CPU to hear the answer, commit, and
	//    close. Bounded: a logd that died already has a claim that lapses by
	//    heartbeat, and a boot that never ran one holds no claim at all —
	//    either way this loop exits. The extra grace after release covers
	//    the daemon's final close-and-sync landing on disk.
	//    Both waits wear the seatbelt (shutdown_wait, above): "bounded" used
	//    to mean bounded BY THE CLOCK, which is exactly the bound that isn't
	//    one on the machine this descent was hardened for.
	klog_request_retire();
	if (!shutdown_wait(3 * TICKS_PER_SECOND, klog_sink_is_claimed))
		printf("  (the tick clock is not advancing — not waiting on the log daemon)\n");
	shutdown_wait(TICKS_PER_SECOND / 2, NULL);
	printf("  log daemon retired\n");
    printd(DEBUG_SHUTDOWN, "  log daemon retired\n");
    // Whatever the daemon did NOT take, the wire gets — the same emergency
	// drain panic uses. Load-bearing on any boot where LOGD= was set but the
	// daemon never attached (its file's directory missing, say): the kernel
	// drainer holds serial fire waiting for a sink, so without this line the
	// whole boot's log would still be IN THE RINGS at poweroff. This descent
	// violated never-drop-a-byte exactly once, on its very first test flight
	// (2026-08-08), and this call is the scar.
	logd_emergency_flush();

	// 3. Sweep the open-file registry — FAT's true-length commits live here.
	//    (Result via the static above, NOT a stack local — see its comment.)
	sh_synced = -1;
	call_in_kernel_context(shutdown_sync_all_in_kernel_context, NULL);
	printf("  %ld open file(s) synced\n", sh_synced < 0 ? 0L : (long)sh_synced);

    // 4. Tell the drives to make it true on the media.
	call_in_kernel_context(shutdown_flush_in_kernel_context, NULL);
	printf("  storage caches flushed\n");

    // 5. Out. Interrupts off first — nothing below wants preemption.
	__asm__ volatile("cli");

	if (mode == OS64_SHUTDOWN_REBOOT)
	{
		serial_print_string("[shutdown] descent complete, rebooting\n");

		// Three doors, weakest side effect first. 0xCF9 is the ICH/PCH reset
		// control register every chipset since the late 90s decodes: bit 1
		// arms it, bit 2 pulls the reset, and bit 3 asks for a FULL (cold)
		// reset — we ask for warm, which is faster and enough. QEMU decodes
		// it too. 0x64 is the 8042's pulse-output line, the PC's original
		// reset: the keyboard controller was wired to the CPU's RESET pin
		// because in 1984 there was nowhere else to put it, and the trick
		// outlived the reason by forty years.
		outb(0xCF9, 0x02);      // arm
		outb(0xCF9, 0x06);      // reset (warm)
		outb(0x64, 0xFE);       // 8042 pulse — the 1984 fallback

		// Still here? Then nothing decoded either port. A TRIPLE FAULT is the
		// last door and it always opens: load a null IDT and raise an
		// interrupt, and the CPU resets because it has no other choice. Ugly
		// on purpose — this is the "your firmware ignored two standard reset
		// paths" branch, not the normal one.
		struct { uint16_t limit; uint64_t base; } __attribute__((packed)) nullIdt = { 0, 0 };
		__asm__ volatile("lidt %0; int3" :: "m"(nullIdt));
		__builtin_unreachable();
	}

	//    THREE DOORS, most standard first.
	serial_print_string("[shutdown] descent complete, powering off\n");

	//    The hypervisor ports first. They are decoded by QEMU and VBox and by
	//    nothing else, so on a physical machine these two lines are a pair of
	//    no-ops costing nothing — and under a hypervisor they are instant and
	//    proven, which keeps the harness's behaviour exactly as it was.
	outw(0x604, 0x2000);    // QEMU (q35 ACPI PM1a)
	outw(0x4004, 0x3400);   // VirtualBox

	//    Then ACPI soft-off: the real mechanism, and the only one a physical
	//    machine has ever decoded. The numbers were read out of the DSDT at
	//    boot (acpi.h); this spends them. Added 2026-08-21, the day the P5
	//    reached "It is now safe to turn off your computer" and then sat
	//    there being perfectly safe to turn off, by hand, forever.
	//
	//    ORDER NOTE: this is second ONLY because it is the unproven one — the
	//    hypervisor path has years of flights behind it and this has none, so
	//    it goes where a failure costs nothing that was working before. If it
	//    proves itself on iron, it earns first place.
	acpi_poweroff();

	//    Still here: say the eleven words every PC user over forty can
	//    recite from memory, and mean them — after the steps above, the
	//    machine really is safe to switch off.
	printf("\nIt is now safe to turn off your computer.\n");
	while (true)
		__asm__ volatile("hlt");
	__builtin_unreachable();
}
