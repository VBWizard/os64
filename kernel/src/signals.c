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
#include "exception_report.h"   // exception_context_t — SIGSEGV delivery reads the fault frame
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
_Static_assert(SIGWINCH == OS64_SIGWINCH, "SIGWINCH disagrees with the ABI (os64/signal.h)");
_Static_assert(SIGNAL_COUNT == OS64_SIGNAL_COUNT,
               "the signal table size disagrees with the ABI (os64/signal.h)");

// Is this a signal number this kernel ACTUALLY HAS? Membership in the public
// set, listed once (Codex #29 rd13).
//
// Two predicates rather than one, because the ABI has two error codes and they
// mean different things: OS64_SIG_ERR_BAD_SIGNAL is "not a signal number this
// kernel knows" and OS64_SIG_ERR_UNCATCHABLE is "SIGKILL". Answering
// UNCATCHABLE for signal 3 would be claiming 3 is a signal you may not catch,
// when the truth is that 3 is not a signal at all. A caller told the wrong one
// goes looking in the wrong place.
//
// EXPLICITLY ABSENT, and this is the point of the whole function: the
// SCHEDULER MARKERS (SIGHALT/SIGSLEEP/SIGUSLEEP/SIGLOGFLUSH, 24-27). The enum
// says out loud they are "not really signals at all" — they are thread state
// that happens to live in the same word. But `signal_raise(SIGSLEEP, ...)`
// sets a bit in the SAME sigind set delivery scans, so while this was a bare
// range check, ring 3 could install a handler on 25 and then:
//
//   1. sleep, so the scheduler raises SIGSLEEP on this thread,
//   2. have the next dispatcher exit find bit 25 pending, catchable, handled,
//   3. get a frame built and its handler run for "signal 25",
//   4. and signal_mark_delivered CLEARS SIGSLEEP ON EVERY THREAD of the task
//      — deleting the scheduler's own record that they are asleep.
//
// One legal-looking call, and ring 3 is corrupting kernel scheduling state
// through an API that reported success. A number the kernel cannot raise must
// never register.
//
// ALSO ABSENT, AND THIS IS THE HARDER CALL: SIGCONT (18) and SIGSTOP (19).
// Both have a NUMBER and no PRODUCER — nothing in this kernel raises them.
// DEBTS § No job control books their stop/continue semantics and delivery as
// a future slice, calling them exactly what they are: stubs.
//
// SIGCONT and SIGSTOP were in this list until rd14 pointed out that keeping
// them contradicts the rule the rest of this function exists to enforce. A
// handler for a signal nothing can send is a caller waiting forever, and it
// makes no difference whether the reason is "no such number" or "the slice
// has not landed". SIGWINCH (28) sat outside for the same reason until the
// resize slice gave it a producer (syscall_pty_resize) — the day a signal
// gets a sender is the day it joins the list, which is exactly the rule.
//
// THE COUNTER-ARGUMENT, considered and rejected — and it deserved considering,
// because os64 has made exactly this bet before and WON it: registration
// shipped before delivery for the main signals on purpose, so a program
// written against it started working the day delivery arrived, without
// changing a line. The difference is that those signals had a producer and a
// default action the entire time; an installed handler already meant "do not
// apply the default action", which was real behaviour on day one. SIGCONT and
// SIGSTOP have no producer, no default and no semantics, so accepting one buys
// the caller nothing and costs it the truth. They rejoin this list the day the
// job-control slice gives them meaning, and a program that checks the return
// simply starts succeeding.
//
// Until then OS64_SIG_ERR_BAD_SIGNAL is the honest answer: "not a signal
// number this kernel knows" is exactly the situation.
bool signal_is_known(signals sig)
{
	switch (sig)
	{
		// EVERY ENTRY NAMES ITS PRODUCER, and that is a rule, not decoration
		// (Codex #29 rd15). This list has now been trimmed BY HAND three
		// times — rd13 took out the numeric gaps and the scheduler markers,
		// rd14 took out SIGCONT and SIGSTOP, rd15 took out SIGIO — because it
		// was written as a list of names and checked case by case, so each
		// pass found only what it happened to look at. A list that states WHO
		// RAISES each member cannot hide the next one: an entry with no
		// producer to name is visibly wrong to anybody reading it, including
		// whoever is adding one.
		case SIGHUP:   // tty_task_departed — the seated shell went away
		case SIGINT:   // console Ctrl+C; /proc/<id>/ctl "interrupt"
		case SIGKILL:  // /proc/<id>/ctl "kill"; task_terminate_sibling_threads
		case SIGSEGV:  // the page-fault handler, on an unresolvable user fault
		case SIGPIPE:  // syscall_write, into a pipe whose readers have all gone
		case SIGTERM:  // the shutdown ladder
		case SIGWINCH: // syscall_pty_resize, at every seat of the slave that has a handler
			return true;
		default:
			// Gaps, scheduler markers — and NUMBERED-BUT-NOT
			// YET-REAL: SIGCONT/SIGSTOP (job control is booked, not built) and
			// SIGIO (nothing in kernel or userland raises it; it was claimed
			// alongside the POSIX numbers and never given a sender). Each
			// rejoins the list above the day something can send it, and brings
			// its producer comment with it.
			return false;
	}
}

bool signal_is_catchable(signals sig)
{
	// Was a bare range check ("1 <= sig < SIGNAL_COUNT && sig != SIGKILL") until
	// rd13. See signal_is_known above for what that let through and why it
	// mattered — this function is consulted by REGISTRATION and by BOTH delivery
	// pickers (signal_pick_deliverable, signal_has_handler_for_pending), which
	// is exactly why it was extracted into one place: fixing membership here
	// shuts every door at once.
	return signal_is_known(sig) && sig != SIGKILL;
}

// Install a handler on the TASK (see task.h for why it is the task's and not
// the thread's) and answer with the one it replaced. NULL restores the
// kernel's default action.
void *signal_set_handler(struct task *t, signals sig, void *handler)
{
	task_t *task = (task_t *)t;
	// UNDER signalLock (Codex #29 rd6): registration is the THIRD writer of a
	// task's signal state — delivery CONSUMES pending bits (rd2's lock),
	// publication SETS them (rd4's lock), and this sets the HANDLER. Left
	// unlocked, a delivery path could read a non-NULL handler in
	// signal_pick_deliverable and then read a just-installed NULL when it
	// copies the handler into the frame, so the trampoline would call address
	// 0 and kill the program instead of applying the default action. Holding
	// the lock keeps the handler stable across every LOCKED delivery (§5/§10,
	// which read it inside their own signalLock section). The SIGSEGV path
	// (§9) reads the handler BEFORE it takes the lock — it takes signalLock
	// too, since rd8, but for page lifetime across the frame writes, not for
	// the handler — so it snapshots the pointer once, up front, for the same
	// end. (This line said "lock-free by nature" until Codex #29 rd16, which
	// was true of the pending set and false of the function — the same
	// half-truth §9's own header had already corrected.)
	uint64_t f = spinlock_acquire_irqsave(&task->signalLock);
	void *previous = task->sighandler[sig];
	task->sighandler[sig] = handler;
	spinlock_release_irqrestore(&task->signalLock, f);
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
// Ring 3 controls the stack pointer, hence frame_va, hence the address this
// writes to — so the target is RESOLVED THROUGH THE USER-WRITABLE GUARD
// (paging_resolve_user_writable, Codex #29 rd2), never merely walked. A
// forged RSP aimed at the kernel's upper-half (mapped in every task PML4)
// would otherwise make delivery a ring-3 -> ring-0 write, and one aimed at
// libos64.so's read-only shared text would corrupt every process at once.
// Every 8-byte write is guarded on its own page, so a frame straddling into
// a bad page fails that write and the whole delivery is abandoned (the
// existing `ok &=` / FAILED path).
static bool signal_write_user(task_t *task, uint64_t user_va, uint64_t value)
{
	uintptr_t phys = paging_resolve_user_writable((pt_entry_t *)task->pml4v, user_va);
	if (phys == 0)
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

	// TWO ANSWERS, AND THE ORDER BETWEEN THEM IS THE POINT (Codex #29 rd17).
	// This loop used to return true at the FIRST handled bit, which made a
	// held, handled SIGINT (handler running, a second INT masked) answer
	// "caught" for the whole thread — and an unhandled SIGTERM pending beside
	// it inherited the reprieve: signal_pick_deliverable skips both (INT masked,
	// TERM has no handler), so nothing delivers and nothing dies, until the
	// INT handler returns. If it never does, a signal whose default is death
	// has been postponed indefinitely by a handler for a DIFFERENT signal.
	// Only SIGKILL got through, and TERM is not supposed to need escalating.
	//
	// So: a pending signal with NO handler and a DEATH default is not covered
	// by anyone's reprieve — it answers "no" (apply the default) regardless of
	// what else is pending and handled. Only if every pending death-default
	// signal is handled does a handled one earn the "yes". The ladder in
	// raise_terminating_signal_and_die then names the unhandled one (TERM
	// before INT), so the corpse wears the right code.
	bool caught = false;
	for (int sig = 1; sig < SIGNAL_COUNT; sig++)
	{
		if (!sigset_has(thread->signals.sigind, (signals)sig))
			continue;
		if (!signal_is_catchable((signals)sig))
			continue;
		if (task->sighandler[sig] == NULL)
		{
			if (SIG_BIT(sig) & SIGNALS_DEFAULT_IS_DEATH)
				return false;               // an unhandled death outranks any reprieve
			continue;                       // unhandled, non-fatal: nobody's business here
		}
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
		// "deliverable now vs. held" split at the checkpoints — DEBTS. The
		// wart's reach is now bounded by the rule above: it can hold up the
		// handler's OWN signal, never a different unhandled one.
		caught = true;
	}
	return caught;
}

// See signals.h. The terminate test comes first and unconditionally: a
// pending SIGKILL makes signal_has_handler_for_pending answer "no" by
// design, and the park must still end so the checkpoint can carry out the
// kill. Reads the handler table without the lock, as every checkpoint does —
// a stale answer here costs one extra trip round the park loop, never a
// wrong decision, because the loop asks again and the syscall boundary
// decides under signalLock.
bool signal_park_must_end(void *thrd)
{
	thread_t *thread = (thread_t *)thrd;
	if (thread == NULL)
		return false;
	if (sigset_any(thread->signals.sigind, SIGNALS_TERMINATING))
		return true;
	return signal_has_handler_for_pending((task_t *)thread->ownerTask, thread);
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
		{
			// No handler: the default action stands. For a death-default
			// signal that means the checkpoints (or the orphan check below)
			// end the task. For a default-IGNORE signal, ignoring is spelled
			// CONSUMING — and the FIRST place that happens is publication
			// (task_signal_and_nudge drops a WINCH at a task with no handler
			// before any bit is set, because a thread parked with no handler
			// never comes through here; Codex #32). This is the backstop for
			// the gap between: published while a handler was installed,
			// uninstalled by a sibling before the pick. Cleared from EVERY
			// thread of the task, not only the one passing through here
			// (rd2): the publish was task-wide, and a sibling parked with
			// no handler will not come through here either — a thread-local
			// clear would leave its bit to fire on the next install. Under
			// the lock both delivery paths hold, so nothing sits pending
			// until a handler happens to be installed and then fires for a
			// resize that happened an hour ago.
			if (SIG_BIT(sig) & SIGNALS_DEFAULT_IS_IGNORE)
				for (thread_t *th = task->threads; th != NULL; th = th->taskNext)
					sigset_del(&th->signals.sigind, (signals)sig);
			continue;
		}
		if (sigset_has(thread->signals.sigmask, (signals)sig))
			continue;                       // already inside this signal's own handler
		return sig;
	}
	return 0;
}

// AN ORPHANED DEATH (Codex #29 rd18): a pending signal whose default is death,
// that has NO handler, and that NO CHECKPOINT WILL EVER ACTION. Today that is
// exactly SIGPIPE — deliberately outside SIGNALS_TERMINATING because an
// uncaught SIGPIPE dies at the write() site and never needs noticing later.
// That stopped being the whole story once SIGPIPE became catchable: write()
// publishes the bit (to EVERY thread of the task) and returns, trusting the
// dispatcher exit to arm the handler. Between that publish and this exit,
// a sibling can uninstall the handler — registration takes signalLock, but
// write() has already dropped it. Now the pick finds no handler and skips
// the bit; the checkpoints exclude it; and the writer returns INTERRUPTED
// with nothing run and nothing dead: a stuck pending SIGPIPE, the exact
// livelock rd7 closed by check-and-act under one lock — arriving from the
// other side. Wider than the writer, too: the sibling that got the broadcast
// bit may be SPINNING, making no syscalls, so §10 has to ask as well.
//
// Both delivery paths ask this when the pick answers 0, still under the
// task's signalLock, so registration cannot move the handler under the
// check. "Yes" becomes SIGNAL_DELIVER_FAILED, which every caller already
// turns into the default action (the ladder has a SIGPIPE arm since rd9).
static bool signal_orphaned_death_pending(task_t *task, thread_t *thread)
{
	for (int sig = 1; sig < SIGNAL_COUNT; sig++)
	{
		if (!sigset_has(thread->signals.sigind, (signals)sig))
			continue;
		if (!signal_is_catchable((signals)sig))
			continue;                       // SIGKILL: the checkpoints own it
		if (task->sighandler[sig] != NULL)
			continue;                       // handled (or held): not orphaned
		if ((SIG_BIT(sig) & SIGNALS_DEFAULT_IS_DEATH) &&
		    !(SIG_BIT(sig) & SIGNALS_TERMINATING))
			return true;
	}
	return false;
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
		// Nothing to arm — but is something pending that nobody will ever
		// act on? (rd18, see signal_orphaned_death_pending.) FAILED sends the
		// caller to the default action; the ladder names SIGPIPE.
		bool orphaned = signal_orphaned_death_pending(task, thread);
		spinlock_release_irqrestore(&task->signalLock, sig_flags);
		return orphaned ? SIGNAL_DELIVER_FAILED : SIGNAL_DELIVER_NONE;
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
		// SIGNALS_DEFAULT_IS_DEATH, not SIGNALS_TERMINATING (Codex #29 rd9):
		// the question here is "what happens when the handler cannot run?",
		// which is about the DEFAULT ACTION — not "would a checkpoint kill
		// for this?", which is what the other mask answers. SIGPIPE is the
		// signal where the two disagree, and asking the wrong one let a
		// process survive a SIGPIPE purely because its handler could not be
		// delivered. (signals.h carries the argument for both masks.)
		if (SIG_BIT(sig) & SIGNALS_DEFAULT_IS_DEATH)
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
	// Enter the handler with DF CLEAR (Codex #29 rd10, signals.h carries the
	// argument). frame[0] is the LIVE rflags sysretq will resume on; the
	// interrupted value is already safe in the frame's saved copy at +32,
	// written from user_rfl above, so sigreturn still restores it exactly.
	frame[0] &= ~SIGNAL_RFLAGS_DF;

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
// ancestor. THREE things change in regs: RIP (to the stub), RSP (to the
// frame), and RFLAGS (DF cleared, rd10). The stub takes signo and handler
// from the frame, and sigreturn's full path puts everything else back from
// the frame too.
//
// The CALLER (scheduler_signal_visit) mirrors ALL THREE — regs.RIP, regs.RSP
// and regs.RFLAGS — into the per-core isr arrays when the thread is staying
// on its CPU, because the continue path resumes from those without a
// reload. Both images, always: the forced push's own discipline, inherited
// whole. (This paragraph said "only RIP and RSP" until rd25 — the last
// spelling of the two-register model that rd11 found had made the DF clear
// a no-op on exactly that path. Whatever delivery changes, the mirror
// carries; a partial mirror looks maintained and is not.)
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
		// Same orphan question as §5 (rd18): a spinner can hold a broadcast
		// SIGPIPE bit whose handler a sibling has since removed, and it makes
		// no syscall at which to notice. FAILED sends the caller to the
		// gallows; the victim dies at its next dispatcher exit by the ladder.
		bool orphaned = signal_orphaned_death_pending(task, thread);
		spinlock_release_irqrestore(&task->signalLock, sig_flags);
		return orphaned ? SIGNAL_DELIVER_FAILED : SIGNAL_DELIVER_NONE;
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
		// Same mask, same reasoning as §5's twin block above (rd9): the
		// default action decides, and SIGPIPE's default is death.
		if (SIG_BIT(sig) & SIGNALS_DEFAULT_IS_DEATH)
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
	// DF clear on entry, same rule as §5 — and here it matters slightly more,
	// because a thread interrupted mid-spin can be at ANY instruction, string
	// op included. The saved copy at +32 already holds the spin's own flags.
	thread->regs.RFLAGS &= ~SIGNAL_RFLAGS_DF;

	printd(DEBUG_SIGNALS,
	       "signal_deliver_to_regs: %s runs handler %p for signal %d (spinner resumes %p)\n",
	       task->exename, task->sighandler[sig], sig, (void *)spin_rip);
	return SIGNAL_DELIVER_ARMED;
}

// ── SIGSEGV DELIVERY: from the PAGE-FAULT handler (SIGNALS.md §9) ────────────
//
// The acceptance test of the whole arc — Chris's os32 app that faults on
// purpose and brags about catching it. This delivery is unlike §5 and §10 in
// three ways, all because a fault is SYNCHRONOUS and THREAD-LOCAL:
//
//  - The target is the FAULTING thread itself, not a broadcast, so there is
//    no task-wide sigind bit to set or clear: nothing but this thread touches
//    this thread's mask. It still takes signalLock, but for the OTHER thing
//    that lock guards (Codex #29 rd8) — the frame writes below reach a user
//    page through its HHDM alias, and a sibling calling unmap() could free
//    that page between a resolve and its store, turning a ring-3 race into a
//    ring-0 #PF. See task.h; syscall_unmap holds the same lock per page.
//    (An earlier version of this comment said §9 had "no lock to take". That
//    was true of the PENDING SET and false of page lifetime, which is
//    exactly the kind of half-truth that leaves a hole.)
//  - The interrupted state lives in the EXCEPTION frame (ctx, built on the
//    stack by exception_entry.S), not in a syscall frame or thread->regs. We
//    build the full frame from ctx and redirect ctx->rip/ctx->rsp — and the
//    exception's own iretq resumes into the stub, because exception_entry.S
//    restores the GP registers from ctx and iretqs its rip/rsp (its comment
//    says the handler is allowed to edit the context; this is that handler).
//  - The saved RIP is the FAULTING instruction. A handler that simply returns
//    resumes it and faults again — so a real SIGSEGV handler exits or longjmps
//    (the fixture exits). A handler that instead FAULTS is caught by the mask
//    below: SIGSEGV is blocked for the duration of its own handler (§7), so a
//    fault inside it finds the mask set, delivery is refused, and the task
//    dies — no infinite fault loop.
//
// Returns true if the handler was armed (ctx now runs the stub, the caller
// must RESUME); false if nothing catches it (no handler, the handler itself
// faulted, or — §9's one honest limit — the stack that faulted cannot hold
// the frame), in which case the caller kills the task exactly as before.
bool signal_deliver_segv(struct task *t, void *thrd, void *context)
{
	task_t              *task   = (task_t *)t;
	thread_t            *thread = (thread_t *)thrd;
	exception_context_t *ctx    = (exception_context_t *)context;

	if (task == NULL || thread == NULL || ctx == NULL || task->kernelTask)
		return false;
	if (!signal_is_catchable(SIGSEGV))
		return false;
	// ONE snapshot of the handler, used for both the guard AND the frame write
	// (Codex #29 rd6): registration runs unlocked-relative-to-us, so reading
	// task->sighandler[SIGSEGV] twice could see non-NULL here and NULL at the
	// frame, calling address 0. A snapshot that goes stale is benign — the
	// function it points at still exists (uninstalling does not unmap it), so
	// a signal already in flight simply runs the handler one last time.
	void *segv_handler = task->sighandler[SIGSEGV];
	if (segv_handler == NULL)
		return false;                       // no handler: the default (139) stands
	if (sigset_has(thread->signals.sigmask, SIGSEGV))
		return false;                       // the SIGSEGV handler itself faulted — let it die (§7)

	// Full frame below the fault's own RSP, respecting the red zone, from ctx.
	uint64_t frame_va = (ctx->rsp - 128 - sizeof(signal_frame_full_t)) & ~(uint64_t)0xF;

	// The page-lifetime barrier (see the header comment and task.h): held
	// across every frame write, so no sibling's unmap() can free a page
	// between one write's resolve and its store.
	uint64_t segv_flags = spinlock_acquire_irqsave(&task->signalLock);

	bool ok = true;
	ok &= signal_write_user(task, frame_va + 0,   SIGNAL_FRAME_MAGIC_FULL);
	ok &= signal_write_user(task, frame_va + 8,   ctx->rax);
	ok &= signal_write_user(task, frame_va + 16,  ctx->rip);   // the faulting instruction
	ok &= signal_write_user(task, frame_va + 24,  ctx->rsp);
	ok &= signal_write_user(task, frame_va + 32,  ctx->rflags);
	ok &= signal_write_user(task, frame_va + 40,  (uint64_t)SIGSEGV);
	ok &= signal_write_user(task, frame_va + 48,  (uint64_t)segv_handler);
	ok &= signal_write_user(task, frame_va + 56,  0);
	ok &= signal_write_user(task, frame_va + 64,  ctx->rbx);
	ok &= signal_write_user(task, frame_va + 72,  ctx->rcx);
	ok &= signal_write_user(task, frame_va + 80,  ctx->rdx);
	ok &= signal_write_user(task, frame_va + 88,  ctx->rsi);
	ok &= signal_write_user(task, frame_va + 96,  ctx->rdi);
	ok &= signal_write_user(task, frame_va + 104, ctx->rbp);
	ok &= signal_write_user(task, frame_va + 112, ctx->r8);
	ok &= signal_write_user(task, frame_va + 120, ctx->r9);
	ok &= signal_write_user(task, frame_va + 128, ctx->r10);
	ok &= signal_write_user(task, frame_va + 136, ctx->r11);
	ok &= signal_write_user(task, frame_va + 144, ctx->r12);
	ok &= signal_write_user(task, frame_va + 152, ctx->r13);
	ok &= signal_write_user(task, frame_va + 160, ctx->r14);
	ok &= signal_write_user(task, frame_va + 168, ctx->r15);
	if (!ok)
	{
		// §9's honest limit: the stack itself is what faulted, so there is
		// nowhere to put the frame. Refuse — the caller kills the task (139),
		// exactly as it did before handlers existed. An alternate signal
		// stack is the cure, and it is a later slice.
		printd(DEBUG_SIGNALS,
		       "signal_deliver_segv: %s has no usable stack for the frame — dies (139)\n",
		       task->exename);
		spinlock_release_irqrestore(&task->signalLock, segv_flags);
		return false;
	}

	// Block SIGSEGV for the duration of its own handler (§7). No sigind bit:
	// SIGSEGV is synchronous, not a queued broadcast — the mask is what
	// sigreturn checks to accept the frame and what turns a re-entrant fault
	// into a clean death instead of an endless loop.
	sigset_add(&thread->signals.sigmask, SIGSEGV);
	spinlock_release_irqrestore(&task->signalLock, segv_flags);

	uint64_t fault_rip = ctx->rip;
	ctx->rip = TASK_SIGRETURN_VIRT;
	ctx->rsp = frame_va;
	// DF clear on entry, same rule as §5 and §10. The exception's own iretq
	// resumes on ctx->rflags, and the faulting instruction's real flags are
	// already in the frame's saved copy at +32 for sigreturn to restore.
	ctx->rflags &= ~SIGNAL_RFLAGS_DF;

	printd(DEBUG_SIGNALS,
	       "signal_deliver_segv: %s catches SIGSEGV, handler %p (faulted at %p)\n",
	       task->exename, segv_handler, (void *)fault_rip);
	return true;
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

		if (signal_park_must_end(qSleep))
		{
			// A pending terminate — or a pending signal a handler will catch
			// — outranks the nap. Cancel the sleep and wake the thread INTO
			// its own blocking loop (console_read / pipe_read / pipe_write
			// ...), whose top-of-loop check asks the same question and bails
			// out to the syscall boundary: a terminate nothing catches gets
			// its default action (130) enforced in the dying task's own
			// context, free to sleep and to close handles; a caught signal
			// gets its handler armed at the dispatcher exit and the call
			// answers INTERRUPTED. This wake is what makes Ctrl+C (and a ctl
			// write, and a window resize) reach a task that is blocked and
			// would otherwise never run again to notice it. (Lock order holds:
			// the predicate reads the handler table lock-free, like every
			// checkpoint, and we hold only the queue lock here.)
			qSleep->signals.sigdata[SIGSLEEP] = 0;
			sigset_del(&qSleep->signals.sigind, SIGSLEEP);
			scheduler_change_thread_queue_locked(qSleep, THREAD_STATE_RUNNABLE);   //we hold the queue lock (above)
			printd(DEBUG_SCHEDULER, "\tThread 0x%08x awoken from ISLEEP by a pending signal\n", qSleep->threadID);
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
