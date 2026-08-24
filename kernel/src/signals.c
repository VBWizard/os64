#include "signals.h"
#include "CONFIG.h"
#include "task.h"        // task_t — the handler table lives there now
#include "os64/signal.h" // the ABI numbers these must agree with
#include "memory/paging.h"   // paging_walk_paging_table / kHHDMOffset — the
                             // HHDM write is how the kernel reaches a user
                             // stack without switching CR3 (CLAUDE.md)
#include <stddef.h>          // offsetof — the stub's ABI asserts
#include "scheduler.h"
#include "spinlock.h"   // task->signalLock — one delivery at a time per task
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

	// A pending SIGKILL poisons the whole answer: NOTHING is "caught" while
	// the last resort is aboard. Without this, a handler for some sibling
	// signal (installed, or masked-and-held) would answer "yes, caught" and
	// the kill checkpoints would defer — a SIGKILL a handler chain can
	// postpone is not a last resort. The same rule lives in
	// signal_pick_deliverable so neither delivery path arms a handler in
	// front of a kill.
	if (sigset_has(thread->signals.sigind, SIGKILL))
		return false;

	for (int sig = 1; sig < SIGNAL_COUNT; sig++)
	{
		if (!sigset_has(thread->signals.sigind, (signals)sig))
			continue;
		if (!signal_is_catchable((signals)sig))
			continue;
		// A MASKED signal still counts as caught. The mask means "inside this
		// signal's own handler" (§7), and the held bit WILL deliver — at the
		// dispatcher exit right after sigreturn unblocks it. The first version
		// of this loop skipped masked signals entirely and thereby answered
		// "no" for them, and the caller's next move on "no" is the default
		// action: a second Ctrl+C arriving DURING the handler executed the
		// program mid-handler — precisely the program that asked to be told.
		// (The wide-open case is the arc's own poster child: a SIGTERM
		// handler saving unsaved work while the shutdown ladder is entitled
		// to send another.)
		//
		// Known wart, accepted with eyes open: a HANDLER that itself blocks
		// while its own signal is pending again gets INTERRUPTED from every
		// blocking call until it returns (the held bit reads as a pending
		// terminate to the park loops, and now reads as caught here). Honest,
		// rare, and strictly better than being killed; the cure is a
		// "deliverable now vs. held" split at the checkpoints — DEBTS.
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
// The full frame (§10) leans on the same stub, so its base must BE the §5
// frame — first field, offset zero, no padding in front.
_Static_assert(offsetof(signal_frame_full_t, base) == 0,  "the full frame must start with the §5 frame");
_Static_assert(offsetof(signal_frame_full_t, rbx) == 64,  "the full frame's extension must start at +64");
_Static_assert(sizeof(signal_frame_full_t) % 16 == 0,     "the full frame must keep RSP 16-aligned");

// Which pending signal, if any, would a handler run for RIGHT NOW? One
// policy, shared by both delivery paths (§5's dispatcher exit and §10's
// scheduler visit) so they can never disagree about who is next: lowest
// number first — a stable order beats an arbitrary one, and "lowest first"
// puts SIGHUP ahead of SIGTERM, which is the order the world tends to send
// them in anyway. Masked signals are HELD (they deliver after sigreturn
// unmasks), SIGKILL never answers, no-handler bits are the default action's
// business. Returns 0 when nothing is deliverable.
//
// AND NOTHING DELIVERS IN FRONT OF A SIGKILL: a pending kill answers 0
// unconditionally, because arming a handler first would let a handler that
// never returns postpone the one signal that must always work. (Its twin
// lives in signal_has_handler_for_pending, which answers "no" for the same
// reason — the two halves keep the kill checkpoints and the delivery paths
// telling one story.)
static int signal_pick_deliverable(task_t *task, thread_t *thread)
{
	if (sigset_has(thread->signals.sigind, SIGKILL))
		return 0;

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
		return sig;
	}
	return 0;
}

// The bookkeeping both delivery paths share, done ONCE per delivery:
// consume the bit task-wide (the aim is a broadcast — leaving it on the
// siblings would run one SIGTERM once per thread, the exact outcome the
// per-task handler table exists to prevent, §2/§3) and block the signal for
// the duration of its own handler (§7; sigreturn lifts it).
static void signal_mark_delivered(task_t *task, thread_t *thread, int sig)
{
	for (thread_t *th = task->threads; th != NULL; th = th->taskNext)
		sigset_del(&th->signals.sigind, (signals)sig);
	sigset_add(&thread->signals.sigmask, (signals)sig);
}

signal_deliver_result_t signal_deliver_pending(struct task *t, void *thrd, uint64_t retval)
{
	task_t   *task   = (task_t *)t;
	thread_t *thread = (thread_t *)thrd;

	if (task == NULL || thread == NULL || task->kernelTask)
		return SIGNAL_DELIVER_NONE;

	// Only from a syscall return. A checkpoint reached any other way has no
	// frame to rewrite, and syscall.S clears this on the way out precisely so
	// a stale one can never be mistaken for ours. THE THREAD'S OWN field, not
	// a per-core slot: a blocking syscall parks with its frame live, so a
	// per-core slot would hand us whatever thread last entered a syscall on
	// this core — and rewriting a parked STRANGER's return frame is how an
	// innocent program comes to resume inside our stub (see thread.h).
	uint64_t *frame = (uint64_t *)thread->syscall_return_frame;
	if (frame == NULL)
		return SIGNAL_DELIVER_NONE;

	// One handled signal per visit, chosen by the shared policy (see
	// signal_pick_deliverable — §5 and §10 must never disagree about who is
	// next). The rest of a multi-signal backlog delivers one syscall at a
	// time, each sigreturn's own dispatcher exit arming the next.
	//
	// PICK-BUILD-CONSUME UNDER THE PER-TASK LOCK (Codex #29): the pending bit
	// is a broadcast, so a sibling on another core could pick the same signal
	// and run the handler a second time. The lock makes the claim atomic per
	// task; the frame is built on THIS thread's own user stack, so holding it
	// across the HHDM writes serializes only what must be (the shared pending
	// set), never two threads' distinct stacks.
	uint64_t sig_flags = spinlock_acquire_irqsave(&task->signalLock);
	int sig = signal_pick_deliverable(task, thread);
	if (sig == 0)
	{
		spinlock_release_irqrestore(&task->signalLock, sig_flags);
		return SIGNAL_DELIVER_NONE;
	}

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
		// The stack is unusable — the SIGSEGV-on-a-bad-stack case named
		// in SIGNALS.md §9. A TERMINATING signal reports FAILED so the
		// dispatcher applies the DEFAULT ACTION: returning "nothing
		// happened" would leave the bit pending with a handler installed,
		// and every checkpoint would then answer "it will be caught"
		// forever — a signal that neither delivers nor kills, a livelock
		// only SIGKILL ends. A NON-terminating one is DROPPED (cleared,
		// with a log line): its default is not death, and leaving it
		// pending buys the same livelock with nothing to end it. An
		// alternate signal stack is the cure for the delivery itself, and
		// it is a later slice.
		printd(DEBUG_SIGNALS,
		       "signal_deliver: %s has no usable stack for signal %d — default action stands\n",
		       task->exename, sig);
		if (SIG_BIT(sig) & SIGNALS_TERMINATING)
		{
			spinlock_release_irqrestore(&task->signalLock, sig_flags);
			return SIGNAL_DELIVER_FAILED;
		}
		for (thread_t *th = task->threads; th != NULL; th = th->taskNext)
			sigset_del(&th->signals.sigind, (signals)sig);
		spinlock_release_irqrestore(&task->signalLock, sig_flags);
		return SIGNAL_DELIVER_NONE;
	}

	// Consume task-wide + block for the handler's duration — the shared
	// bookkeeping (see signal_mark_delivered for the §2/§3/§7 story).
	signal_mark_delivered(task, thread, sig);
	spinlock_release_irqrestore(&task->signalLock, sig_flags);

	// Redirect the return: into the stub, standing on the frame we just
	// built. The stub loads the handler out of the frame and calls it. This
	// touches only THIS thread's own return frame, so it is safe outside the
	// lock (which guards the shared pending set, now consumed).
	frame[1] = TASK_SIGRETURN_VIRT;
	frame[2] = frame_va;

	printd(DEBUG_SIGNALS, "signal_deliver: %s runs handler %p for signal %d (resumes %p)\n",
	       task->exename, task->sighandler[sig], sig, (void *)user_rip);
	return SIGNAL_DELIVER_ARMED;
}

// ── §10 DELIVERY: to a thread the SCHEDULER caught spinning in ring 3 ───────
//
// The syscall path (§5, above) arms a handler by rewriting where a syscall
// returns. A program that never makes a syscall has no such moment — but the
// scheduler visits every thread (the BSP ticks; a tickless AP's backstop
// lease arms per non-idle dispatch), and for a thread interrupted in ring 3,
// `thread->regs` ALREADY holds the complete user context the ISR saved.
// Delivery is a rewrite of that saved context: build the FULL frame from
// regs, point regs.RIP at the same stub and regs.RSP at the frame, and the
// thread resumes by iretq into the stub exactly as it would have resumed
// into its spin.
//
// Every register is live at an arbitrary interruption point, so the frame
// carries the whole file (signal_frame_full_t) — os32 delivered from its
// scheduler ISR, and its long stack diagram is this function's direct
// ancestor. Only RIP and RSP change in regs: the stub takes signo and
// handler from the frame, and sigreturn's full path puts everything else
// back from the frame too.
//
// The CALLER (scheduler_signal_visit) mirrors regs.RIP/regs.RSP into the
// per-core isr arrays when the thread is staying on its CPU — the continue
// path resumes from those without a reload. Both images, always: the forced
// push's own discipline, inherited whole.
signal_deliver_result_t signal_deliver_to_regs(struct task *t, void *thrd)
{
	task_t   *task   = (task_t *)t;
	thread_t *thread = (thread_t *)thrd;

	if (task == NULL || thread == NULL || task->kernelTask)
		return SIGNAL_DELIVER_NONE;

	// Same per-task claim as the syscall path (Codex #29): a sibling thread
	// could pick this broadcast signal on another core. We are already under
	// the scheduler queue lock here (IF=0); this nests inside it, and the
	// order is always queue-then-signalLock, so no cycle with the syscall
	// path (which takes signalLock alone).
	uint64_t sig_flags = spinlock_acquire_irqsave(&task->signalLock);
	int sig = signal_pick_deliverable(task, thread);
	if (sig == 0)
	{
		spinlock_release_irqrestore(&task->signalLock, sig_flags);
		return SIGNAL_DELIVER_NONE;
	}

	// Same red-zone respect as §5. Userland is -mno-red-zone today, so the
	// 128 is strictly courtesy — but hand-written ring-3 asm is allowed to
	// exist, and the two delivery paths staying identical is worth more
	// than 128 bytes of stack.
	uint64_t frame_va = (thread->regs.RSP - 128 - sizeof(signal_frame_full_t)) & ~(uint64_t)0xF;

	bool ok = true;
	ok &= signal_write_user(task, frame_va + 0,   SIGNAL_FRAME_MAGIC_FULL);
	ok &= signal_write_user(task, frame_va + 8,   thread->regs.RAX);
	ok &= signal_write_user(task, frame_va + 16,  thread->regs.RIP);
	ok &= signal_write_user(task, frame_va + 24,  thread->regs.RSP);
	ok &= signal_write_user(task, frame_va + 32,  thread->regs.RFLAGS);
	ok &= signal_write_user(task, frame_va + 40,  (uint64_t)sig);
	ok &= signal_write_user(task, frame_va + 48,  (uint64_t)task->sighandler[sig]);
	ok &= signal_write_user(task, frame_va + 56,  0);   // pad — deterministic
	ok &= signal_write_user(task, frame_va + 64,  thread->regs.RBX);
	ok &= signal_write_user(task, frame_va + 72,  thread->regs.RCX);
	ok &= signal_write_user(task, frame_va + 80,  thread->regs.RDX);
	ok &= signal_write_user(task, frame_va + 88,  thread->regs.RSI);
	ok &= signal_write_user(task, frame_va + 96,  thread->regs.RDI);
	ok &= signal_write_user(task, frame_va + 104, thread->regs.RBP);
	ok &= signal_write_user(task, frame_va + 112, thread->regs.R8);
	ok &= signal_write_user(task, frame_va + 120, thread->regs.R9);
	ok &= signal_write_user(task, frame_va + 128, thread->regs.R10);
	ok &= signal_write_user(task, frame_va + 136, thread->regs.R11);
	ok &= signal_write_user(task, frame_va + 144, thread->regs.R12);
	ok &= signal_write_user(task, frame_va + 152, thread->regs.R13);
	ok &= signal_write_user(task, frame_va + 160, thread->regs.R14);
	ok &= signal_write_user(task, frame_va + 168, thread->regs.R15);
	if (!ok)
	{
		// Unusable stack, regs untouched. Same policy as §5's twin block: a
		// TERMINATING signal reports FAILED and the caller falls through to
		// the forced push (death must not depend on the victim's stack); a
		// NON-terminating one is DROPPED here — cleared with a log line —
		// because revisiting it every scheduler pass forever helps nobody.
		printd(DEBUG_SIGNALS,
		       "signal_deliver_to_regs: %s has no usable stack for signal %d\n",
		       task->exename, sig);
		if (SIG_BIT(sig) & SIGNALS_TERMINATING)
		{
			spinlock_release_irqrestore(&task->signalLock, sig_flags);
			return SIGNAL_DELIVER_FAILED;
		}
		for (thread_t *th = task->threads; th != NULL; th = th->taskNext)
			sigset_del(&th->signals.sigind, (signals)sig);
		spinlock_release_irqrestore(&task->signalLock, sig_flags);
		return SIGNAL_DELIVER_NONE;
	}

	signal_mark_delivered(task, thread, sig);
	spinlock_release_irqrestore(&task->signalLock, sig_flags);

	uint64_t spin_rip = thread->regs.RIP;
	thread->regs.RIP = TASK_SIGRETURN_VIRT;
	thread->regs.RSP = frame_va;

	printd(DEBUG_SIGNALS,
	       "signal_deliver_to_regs: %s runs handler %p for signal %d (spinner resumes %p)\n",
	       task->exename, task->sighandler[sig], sig, (void *)spin_rip);
	return SIGNAL_DELIVER_ARMED;
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
