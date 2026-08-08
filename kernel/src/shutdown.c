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

int usedCount=0;
extern volatile uint64_t kSystemCurrentTime;
extern volatile uint64_t kTicksSinceStart;
extern BasicRenderer kRenderer;
extern int kTimeZone;
extern volatile uint64_t kSystemStartTime;
extern task_t* kKernelTask;

// (Named shutdown() from the day this file was born — see shutdown.h for
// why the name finally moved to the function that earns it.)
void kernel_park(void)
{
	uint64_t memInUse=0;
    uint64_t lastTime = 0;
    char heartbeat[2] = "*";
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
        sigaction(SIGSLEEP, NULL, kTicksSinceStart+90,kKernelTask->threads);
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

void shutdown_system(void)
{
	// 1. Announce — glass and wire both (the wire directly: logd is about to
	//    be retired, and this line must survive even if the descent wedges).
	printf("\nThe system is going down NOW!\n");
	serial_print_string("[shutdown] descent started\n");

	// 2. Retire the log daemon. Set the flag, then keep yielding so the
	//    daemon (a ring-3 task) gets CPU to hear the answer, commit, and
	//    close. Bounded: a logd that died already has a claim that lapses by
	//    heartbeat, and a boot that never ran one holds no claim at all —
	//    either way this loop exits. The extra grace after release covers
	//    the daemon's final close-and-sync landing on disk.
	klog_request_retire();
	core_local_storage_t *cls = get_core_local_storage();
	uint64_t deadline = kTicksSinceStart + 3 * TICKS_PER_SECOND;
	while (klog_sink_is_claimed() && kTicksSinceStart < deadline)
		scheduler_trigger(cls);
	uint64_t grace = kTicksSinceStart + TICKS_PER_SECOND / 2;
	while (kTicksSinceStart < grace)
		scheduler_trigger(cls);
	printf("  log daemon retired\n");

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

	// 5. Out. Interrupts off first — nothing below wants preemption. The
	//    hypervisor poweroff ports are harmless no-ops where they don't
	//    apply (nothing decodes them on bare metal), so try both, then say
	//    the eleven words every PC user over forty can recite from memory.
	__asm__ volatile("cli");
	serial_print_string("[shutdown] descent complete, powering off\n");
	outw(0x604, 0x2000);    // QEMU (q35 ACPI PM1a)
	outw(0x4004, 0x3400);   // VirtualBox
	printf("\nIt is now safe to turn off your computer.\n");
	while (true)
		__asm__ volatile("hlt");
	__builtin_unreachable();
}