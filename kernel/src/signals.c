#include "signals.h"
#include "CONFIG.h"
#include "task.h"        // task_t — the handler table lives there now
#include "os64/signal.h" // the ABI numbers these must agree with
#include "memory/paging.h"   // paging_walk_paging_table / kHHDMOffset — the
                             // HHDM write is how the kernel reaches a user
                             // stack without switching CR3 (CLAUDE.md)
#include <stddef.h>          // offsetof — the stub's ABI asserts
#include "scheduler.h"
#include "kernel.h"
#include "serial_logging.h"
#include "panic.h"
#include "thread.h"
#include "smp_core.h"
#include "console.h"
#include "tty.h"     // tty_summon_wake — rousting kworker for a knocked terminal
#include "pipe.h"
#include "thread_join.h"
#include "BasicRenderer.h"   // renderer_flush_if_dirty — the blit-throttle rider
#include "driver/system/usb/xhci.h"
#include "driver/net/virtio_net.h"
#include "driver/net/e1000.h"
#include "driver/net/r8125.h"
#include "driver/net/dhcp.h"
#include "driver/net/udp_conn.h"
#include "driver/net/tcp.h"
#include "driver/net/icmp_conn.h"

extern volatile int kSchedulerSwitchTasksLock;
bool kProcessSignals = false;
uint8_t signalProcTickFrequency;

// ── THE TWO RINGS AGREE ABOUT THE NUMBERS, OR THE BUILD STOPS ───────────────
//
// ring 3 cannot include this header, so os64/signal.h carries its own copy of
// the numbers for programs to use. Two copies of a numbering is exactly how a
// SIGTERM comes to be delivered as a SIGSEGV one refactor from now, so the
// copies are checked against each other HERE, at compile time.
//
// This is klog_format.h's discipline, applied to the second thing that has to
// cross the ring boundary intact: there, renaming a DEBUG_* bit without
// updating the ABI table stops the build rather than mislabelling the log.
// Same rule, higher stakes — a mislabelled log line is a puzzle, a
// misdelivered signal is a program running the wrong handler.
_Static_assert(SIGHUP  == OS64_SIGHUP,  "SIGHUP disagrees with the ABI (os64/signal.h)");
_Static_assert(SIGINT  == OS64_SIGINT,  "SIGINT disagrees with the ABI (os64/signal.h)");
_Static_assert(SIGKILL == OS64_SIGKILL, "SIGKILL disagrees with the ABI (os64/signal.h)");
_Static_assert(SIGSEGV == OS64_SIGSEGV, "SIGSEGV disagrees with the ABI (os64/signal.h)");
_Static_assert(SIGPIPE == OS64_SIGPIPE, "SIGPIPE disagrees with the ABI (os64/signal.h)");
_Static_assert(SIGTERM == OS64_SIGTERM, "SIGTERM disagrees with the ABI (os64/signal.h)");
_Static_assert(SIGCONT == OS64_SIGCONT, "SIGCONT disagrees with the ABI (os64/signal.h)");
_Static_assert(SIGSTOP == OS64_SIGSTOP, "SIGSTOP disagrees with the ABI (os64/signal.h)");
_Static_assert(SIGIO   == OS64_SIGIO,   "SIGIO disagrees with the ABI (os64/signal.h)");
_Static_assert(SIGNAL_COUNT == OS64_SIGNAL_COUNT,
               "the signal table size disagrees with the ABI (os64/signal.h)");

// Can ring 3 install a handler for this signal? The range check and the one
// exception, in one place so registration and (later) delivery cannot come to
// different conclusions about the same signal.
bool signal_is_catchable(signals sig)
{
	if ((int)sig <= 0 || (int)sig >= SIGNAL_COUNT)
		return false;
	// SIGKILL is the answer to a program that has stopped answering. A kernel
	// that let a program decline to die has no last resort — so this is the
	// one signal whose default action cannot be replaced. (SIGSTOP will join
	// it the day job control lands: a process must not be able to refuse to
	// be stopped either, for the same reason.)
	return sig != SIGKILL;
}

// Install a handler on the TASK (see task.h for why it is the task's and not
// the thread's) and answer with the one it replaced. NULL restores the
// kernel's default action.
void *signal_set_handler(struct task *t, signals sig, void *handler)
{
	task_t *task = (task_t *)t;
	void *previous = task->sighandler[sig];
	task->sighandler[sig] = handler;
	printd(DEBUG_SIGNALS, "signal_set_handler: %s installs %p for signal %u (was %p)\n",
	       task->exename, handler, (unsigned)sig, previous);
	return previous;
}

// ── DELIVERY ────────────────────────────────────────────────────────────────
//
// Run a ring-3 handler by changing where the current syscall RETURNS to. The
// mechanism, end to end:
//
//   1. carve a signal_frame_t out of the user stack and fill it with the four
//      values nothing else will restore (see signals.h for why only four);
//   2. point the syscall's saved RIP at the handler, RDI at the signal number
//      (a handler is an ordinary C function taking an int);
//   3. put TASK_SIGRETURN_VIRT at [new RSP] so the handler's own `ret` lands
//      in the stub that calls sigreturn;
//   4. return to user normally. The handler runs on the thread's own stack.
//
// EVERY WRITE GOES THROUGH THE HHDM, never through the task VA. The user stack
// is mapped only in the task's own PML4 and we may not be on that CR3; the
// house rule (CLAUDE.md) is to walk the task's tables for the physical page
// and write through `phys | kHHDMOffset`. This is also what lets os64 skip the
// CR3 juggling os32's _sigJumpPoint needed — there is no address space to
// switch INTO, because the write never uses the task's address space.
//
// The RED ZONE is respected: SysV reserves 128 bytes below RSP that a leaf
// function may be using right now, so the frame starts below it.
static bool signal_write_user(task_t *task, uint64_t user_va, uint64_t value)
{
	uintptr_t phys = paging_walk_paging_table((pt_entry_t *)task->pml4v, user_va);
	if (phys == 0 || phys == 0xbadbadba)
		return false;
	*(uint64_t *)(phys | kHHDMOffset) = value;
	return true;
}

// Does this thread have a pending signal that something will CATCH? The
// default-action checkpoints ask before they kill: a task that installed a
// handler must not be executed by the kernel on its way to being handed the
// signal it asked for.
//
// SIGKILL is never caught, so a pending SIGKILL always answers false here and
// the kill proceeds — which is the property that keeps it the last resort.
bool signal_has_handler_for_pending(struct task *t, void *thrd)
{
	task_t   *task   = (task_t *)t;
	thread_t *thread = (thread_t *)thrd;

	if (task == NULL || thread == NULL)
		return false;

	for (int sig = 1; sig < SIGNAL_COUNT; sig++)
	{
		if (!sigset_has(thread->signals.sigind, (signals)sig))
			continue;
		if (!signal_is_catchable((signals)sig))
			continue;
		if (sigset_has(thread->signals.sigmask, (signals)sig))
			continue;   // inside its own handler; the default must not fire either
		if (task->sighandler[sig] != NULL)
			return true;
	}
	return false;
}

// The stub in task_exit_asm.S reads these offsets off RSP by hand. If the
// struct moves, the stub reads the wrong words and calls whatever happens to
// be in the frame — so the two are held together here rather than by anyone
// remembering.
_Static_assert(offsetof(signal_frame_t, signo)   == 40, "the sigreturn stub reads signo at +40");
_Static_assert(offsetof(signal_frame_t, handler) == 48, "the sigreturn stub reads handler at +48");
_Static_assert(sizeof(signal_frame_t) % 16 == 0,        "the signal frame must keep RSP 16-aligned");

bool signal_deliver_pending(struct task *t, void *thrd, uint64_t retval)
{
	task_t   *task   = (task_t *)t;
	thread_t *thread = (thread_t *)thrd;

	if (task == NULL || thread == NULL || task->kernelTask)
		return false;

	// Only from a syscall return. A checkpoint reached any other way has no
	// frame to rewrite, and syscall.S clears this on the way out precisely so
	// a stale one can never be mistaken for ours.
	core_local_storage_t *cls = get_core_local_storage();
	uint64_t *frame = cls ? (uint64_t *)cls->syscall_return_frame : NULL;
	if (frame == NULL)
		return false;

	// One handled signal, lowest number first — a stable order beats an
	// arbitrary one, and "lowest first" puts SIGHUP ahead of SIGTERM, which
	// is the order the world tends to send them in anyway.
	for (int sig = 1; sig < SIGNAL_COUNT; sig++)
	{
		if (!sigset_has(thread->signals.sigind, (signals)sig))
			continue;
		if (!signal_is_catchable((signals)sig))
			continue;                       // SIGKILL: the default is the only action
		if (task->sighandler[sig] == NULL)
			continue;                       // no handler: the default action stands
		if (sigset_has(thread->signals.sigmask, (signals)sig))
			continue;                       // already inside this signal's own handler

		uint64_t user_rsp = frame[2];       // [16] in syscall.S's frame
		uint64_t user_rip = frame[1];       // [8]
		uint64_t user_rfl = frame[0];       // [0]

		// Below the RED ZONE — SysV reserves 128 bytes under RSP that a leaf
		// function may be using right now — then down for the frame, then
		// 16-aligned, because the stub CALLs the handler from here.
		uint64_t frame_va = (user_rsp - 128 - sizeof(signal_frame_t)) & ~(uint64_t)0xF;

		// Write it out field by field through the HHDM. If ANY write fails the
		// stack is unusable — which is the SIGSEGV-on-a-bad-stack case named in
		// SIGNALS.md §9 — and we deliver nothing, leaving the default action to
		// kill the thread exactly as it does today. An alternate signal stack
		// is the cure, and it is a later slice.
		bool ok = true;
		ok &= signal_write_user(task, frame_va + 0,  SIGNAL_FRAME_MAGIC);
		ok &= signal_write_user(task, frame_va + 8,  retval);
		ok &= signal_write_user(task, frame_va + 16, user_rip);
		ok &= signal_write_user(task, frame_va + 24, user_rsp);
		ok &= signal_write_user(task, frame_va + 32, user_rfl);
		ok &= signal_write_user(task, frame_va + 40, (uint64_t)sig);
		ok &= signal_write_user(task, frame_va + 48, (uint64_t)task->sighandler[sig]);
		if (!ok)
		{
			printd(DEBUG_SIGNALS,
			       "signal_deliver: %s has no usable stack for signal %d — default action stands\n",
			       task->exename, sig);
			return false;
		}

		// CONSUMED ONCE, TASK-WIDE. The aim is a broadcast
		// (task_signal_all_threads), so leaving the bit set on the siblings
		// would run one SIGTERM once per thread — the exact outcome the
		// per-task handler table exists to prevent (SIGNALS.md §2/§3).
		for (thread_t *th = task->threads; th != NULL; th = th->taskNext)
			sigset_del(&th->signals.sigind, (signals)sig);

		// Blocked for the duration of its own handler: a SIGSEGV handler that
		// faults must not re-enter itself forever (§7). sigreturn lifts it.
		sigset_add(&thread->signals.sigmask, (signals)sig);

		// Redirect the return: into the stub, standing on the frame we just
		// built. The stub loads the handler out of the frame and calls it.
		frame[1] = TASK_SIGRETURN_VIRT;
		frame[2] = frame_va;

		printd(DEBUG_SIGNALS, "signal_deliver: %s runs handler %p for signal %d (resumes %p)\n",
		       task->exename, task->sighandler[sig], sig, (void *)user_rip);
		return true;
	}

	return false;
}

/// RAISE a signal on a thread, with data.
///
/// IT WAS CALLED `sigaction` UNTIL 2026-08-23, and the name was wrong twice
/// over: it sets no action, and 21 of its 22 callers use it to mean "park this
/// thread until tick N". Its `sigAction` parameter was dead — `(void)`-cast at
/// the top, "reserved for real handler registration" — and that registration
/// is now being built properly, as a syscall against the task's own handler
/// table, which needed the name back before the two could stand next to each
/// other without confusing everybody who reads them.
///
/// @param signal the signal NUMBER (signals.h)
/// @param sigData data to accompany it — for SIGSLEEP, the wake tick
/// @param thrd the thread to signal; NULL means the current one
void signal_raise(signals signal, uint64_t sigData, void *thrd)
{
    thread_t *thread = thrd;

	if (thread == NULL)
	{
		core_local_storage_t* cls = get_core_local_storage();
		thread = cls->currentThread;
	}

	switch (signal)
	{
		case SIGSLEEP:
		//Set the data first in case a task switch takes place before setting the sigind	
        thread->signals.sigdata[SIGSLEEP]=sigData;
            sigset_add(&thread->signals.sigind, SIGSLEEP);
            printd(DEBUG_SIGNALS, "Signalling SLEEP for thread 0x%08x, wakeTicks=%i\n", thread->threadID, sigData);
            scheduler_trigger(NULL);
			break;
		case SIGLOGFLUSH:
			sigset_add(&thread->signals.sigind, SIGLOGFLUSH);
			printd(DEBUG_SIGNALS, "Signalling LOGFLUSH for thread 0x%08x\n", thread->threadID);
			scheduler_trigger(NULL);
			break;
		default:
			panic("signal_raise: signal %u has no raise path here\n", signal);
	}
}

//Iterate the running, runnable and sleeping queues, looking for new signals
void processSignals()
{
	thread_t *qSleep = qISleep;

	printd(DEBUG_SIGNALS | DEBUG_DETAILED,"processSignals: Start processing signals\n");
	printd(DEBUG_SIGNALS | DEBUG_DETAILED,"\tScanning Interruptable Sleep queue\n");

	//Set the scheduler task switch lock so that other APs don't see inconsistent state
	while (__sync_lock_test_and_set(&kSchedulerSwitchTasksLock, 1)) __builtin_ia32_pause();
	while (qSleep != NO_THREAD)
	{
		// Capture the neighbor BEFORE any requeue: scheduler_change_thread_queue
		// relinks the node into qRunnable, so following ->next afterward would
		// walk off into the wrong queue mid-iteration.
		thread_t *nextSleeper = qSleep->next;

		if (sigset_any(qSleep->signals.sigind, SIGNALS_TERMINATING))
		{
			// A pending terminate outranks the nap. Cancel the sleep and wake
			// the thread INTO its own blocking loop (console_read / pipe_read /
			// pipe_write), whose top-of-loop terminate check bails out to the
			// syscall boundary — where the default action (terminate, 130) is
			// enforced in the dying task's own context, free to sleep and to
			// close handles. This wake is what makes Ctrl+C (and a ctl write)
			// reach a task that is blocked and would otherwise never run again
			// to notice it.
			qSleep->signals.sigdata[SIGSLEEP] = 0;
			sigset_del(&qSleep->signals.sigind, SIGSLEEP);
			scheduler_change_thread_queue_locked(qSleep, THREAD_STATE_RUNNABLE);   //we hold the queue lock (above)
			printd(DEBUG_SCHEDULER, "\tThread 0x%08x awoken from ISLEEP by a pending terminate\n", qSleep->threadID);
		}
		else if (qSleep->signals.sigdata[SIGSLEEP] <= kTicksSinceStart) // Wake up the thread if the wake time is *now* or in the past
		{
			qSleep->signals.sigdata[SIGSLEEP] = 0;
			sigset_del(&qSleep->signals.sigind, SIGSLEEP);
			scheduler_change_thread_queue_locked(qSleep, THREAD_STATE_RUNNABLE);   //we hold the queue lock (above)
			printd(DEBUG_SCHEDULER, "\tThread 0x%08x awoken from ISLEEP\n", qSleep->threadID);
		}
		qSleep = nextSleeper;
	}

	// Drain USB input BEFORE the console wake check below, so a keyboard HID
	// report that just completed becomes a ring event the same pass its
	// reader wakes for. Polling here rides the exact liveness path the PS/2
	// wake uses — no interrupt wiring, ~one-pass latency. Cheap when idle
	// (guard branch + one cached-RAM read); does nothing before USB init.
	xhci_poll();

	// Same liveness ride for the NIC: drain TX completions and deliver RX
	// arrivals once per pass. Same economics as xhci_poll — a guard branch
	// and two ring-index compares when idle; interrupt wiring is a future
	// slice (NETWORK.md), and this path is what makes packets move today.
	virtio_net_poll();
	// Every registered NIC needs its own drainer — this is a per-DRIVER
	// call, not a per-device one, and each is a guard branch away from free
	// when its hardware is absent. (A generic "poll every net_device" verb
	// through the seam is the obvious tidy-up; it waits for a third driver
	// to make the abstraction pay, the same way the seam itself waited for
	// a second one.)
	//
	// Since 2026-08-06 the e1000 drain is DOORBELL-GATED when INTx is live:
	// the ISR (vector 0x45) raises kE1000RxWork and this pass consumes it.
	// Clear BEFORE draining — a packet that lands mid-drain re-raises the
	// flag and schedules the next pass, instead of vanishing into a
	// cleared-after window (the classic lost-wakeup shape, same reason the
	// wake sweeps re-evaluate conditions instead of remembering edges).
	// No confirmed wire = unconditional poll, yesterday's behavior exactly.
	if (!kE1000UsesIntx || kE1000RxWork)
	{
		// If the ISR divorced the wire at runtime (shared line gone hostile
		// — see storm breaker #2 in e1000.c), announce it exactly once. The
		// fallback itself already happened in interrupt context; this is
		// the no-silent-fallbacks receipt.
		if (kE1000IntxDivorced)
		{
			kE1000IntxDivorced = false;
			printf("e1000: INTx wire went hostile (stranger storm) — back to polling\n");
		}
		kE1000RxWork = false;
		e1000_poll();
	}

	// And the RTL8125's drainer. Wired in from its FIRST slice, before it
	// has any rings to drain, precisely so that adding them needs no
	// scheduler surgery later — same economics as the two calls above: one
	// guard branch away from free when the hardware is absent, which on
	// every machine here except the P5 is always.
	r8125_poll();

	// DHCP's retry timer rides the same pass (one state compare when the
	// lease is settled). Delivery of DHCP replies happens inside the poll
	// above (UDP demux → dhcp_rx); this call only handles the wire going
	// QUIET — resend, and eventually give up honestly.
	dhcp_poll();

	// TCP's clock: retransmission deadlines, connect timeouts, and the
	// TIME_WAIT reaper. THIS is what makes the stream reliable — a stack
	// with no timer is a stack that hangs the first time a packet is
	// lost. Walks only live connections (usually none).
	tcp_poll();

	// Blit-throttle flush: if a console scroll burst left the glass behind
	// its shadow (BasicRenderer's ~30Hz throttle), deliver the finished
	// frame. Costs one branch when the glass is current — which is always,
	// except mid-firehose, which is exactly when it earns its keep.
	renderer_flush_if_dirty();

	// The tty layer's half of the same discipline: a focused-VT scroll burst
	// defers the glass entirely (no per-line memmove — the frozen-cat fix),
	// and this rider repaints from the grid at the same ~30Hz. Ordered after
	// renderer_flush_if_dirty so a frame both riders touch settles in one
	// pass. Costs one branch when nothing is stale.
	tty_flush_if_dirty();

	// Wake any blocked console reader whose terminal has input (one sweep
	// over the tty fleet since VTs arrived). Done here — under the lock,
	// AFTER the qISleep walk above — so its queue surgery (ISLEEP->RUNNABLE)
	// can't corrupt that iteration. Level-triggered on the ring condition,
	// which is what makes console_read lost-wakeup-free.
	console_wake_if_ready();

	// And if a keystroke knocked on a DORMANT terminal, roust kworker early
	// to go play midwife (tty_summon_sweep spawns the shell in task context;
	// kworker's own backstop nap is 2s — fine for burials, rude for a human
	// standing at a dark terminal). Same wake idiom, same lock, same reasons.
	tty_summon_wake();

	// Same discipline, same reason, for pipes: wake any reader whose pipe has
	// bytes (or whose last writer just left — that is its EOF), and any writer
	// whose pipe has room (or whose last reader just left — that is its EPIPE).
	// The fast path already woke the common cases directly from pipe_read /
	// pipe_write; this level-triggered sweep exists to catch the one race they
	// cannot: a thread that registered as a waiter but had not yet parked when
	// the wake fired. Re-evaluating the CONDITION (not a remembered edge) is
	// what makes pipes lost-wakeup-free.
	pipe_wake_if_ready();

	// And for dialed network conversations: wake any reader whose datagram
	// ring is non-empty. Placed AFTER virtio_net_poll above on purpose —
	// a packet delivered this pass wakes its reader this same pass, so
	// blocking-read latency is one scheduler pass, not two. (UDP conns
	// have NO fast-path wake at all — their enqueue runs in RX context,
	// which may not hold this lock — so this sweep is their only waker;
	// see the context map in udp_conn.c.)
	udp_conn_wake_if_ready();

	// Same for TCP streams: a reader wakes for bytes, EOF, or death; a
	// writer wakes when its segment is acknowledged. Same level-triggered
	// re-evaluation, same reason.
	tcp_wake_if_ready();

	// And echo conversations: a reader wakes when its reply lands.
	icmp_conn_wake_if_ready();

	// And threads: wake anyone blocked reading a thread handle whose
	// thread has finished. Same level-triggered discipline — the answer
	// stays true once it exists, so a missed edge costs a tick, never a
	// hang.
	thread_join_wake_if_ready();

	//Release the lock
	__sync_lock_release(&kSchedulerSwitchTasksLock);

	// RFLAGS-tripwire reporter (SCHEDULER_STRAY_WRITE.md): scheduler.S
	// impounds any corrupt mp_isrSavedRFlags value it catches at the iretq
	// and bumps mp_rflagsTripCount; this prints each new impound exactly
	// once. AFTER the lock release on purpose — printing is slow and the
	// evidence isn't going anywhere. DEBUG_EXCEPTIONS because that bit is
	// always-on, and this line existing in a log is the entire point:
	// the raw value names the writer (a .rodata pointer names a table, a
	// .text pointer names a function, a stack address names a frame).
	if (mp_rflagsTripCount != mp_rflagsTripReported)
	{
		mp_rflagsTripReported = mp_rflagsTripCount;
		for (uint32_t core = 0; core < MAX_CPUS; core++)
			if (mp_rflagsTripValue[core] != 0)
			{
				printd(DEBUG_EXCEPTIONS,
					"RFLAGS TRIPWIRE: core %u had mp_isrSavedRFlags = 0x%016lx (sanitized to 0x202; see SCHEDULER_STRAY_WRITE.md)\n",
					core, mp_rflagsTripValue[core]);
				printf("RFLAGS TRIPWIRE: core %u caught the stray write! value 0x%016lx\n",
					core, mp_rflagsTripValue[core]);
				mp_rflagsTripValue[core] = 0;   // impound reported; re-arm the slot
			}
	}

	printd(DEBUG_SIGNALS | DEBUG_DETAILED,"\tprocessSignals: Done processing signals\n");
	//No need to act on "awoken" since processSignals() is called by the scheduler
}

void init_signals()
{
		signalProcTickFrequency = SIGNAL_PROCESS_TICK_FREQUENCY;
}
