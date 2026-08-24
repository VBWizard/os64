#ifndef SIGNALS_H
#define SIGNALS_H

#include <stdint.h>
#include <stdbool.h>

// ── IDENTITY IS A NUMBER; THE PENDING SET IS A BITMASK OF THOSE NUMBERS ─────
//
// Until 2026-08-23 these constants WERE bits, and one name meant both things.
// The header said so itself, as a caution: sighandler[] and sigdata[] were
// indexed by the BIT VALUE, so sigdata[SIGSLEEP] was sigdata[2] — which works
// for bits 0..4 and would have indexed past the end of a 32-entry array for
// the eight signals above them. Twelve signals were defined; eight were above
// bit 4. Ring 3 asking for "a handler on signal X" walks straight into that,
// so the split had to happen before signal delivery could be built at all.
//
// So: a signal IS a number. Its bit is DERIVED, SIG_BIT(sig) == 1u << sig,
// and it lives only in a set. The confusion cannot recur, because the two
// concepts no longer share a spelling — and signal_set_t below makes the
// compiler enforce it rather than the reader.
//
// THE NUMBERS ARE POSIX'S where POSIX has one, kept on merit (DIVERGENCES):
// a corpse tagged 143 is legible to anyone who has ever read a shell's exit
// status, and SIGNALS_EXIT_SIGTERM already encoded 128+15 — the numbers were
// half-adopted before this change, which finishes the job instead of
// inventing a second scheme beside it.
//
// os64's OWN signals take numbers POSIX left free, and three of them are not
// really signals at all: SIGHALT, SIGSLEEP and SIGUSLEEP are scheduler state
// wearing signal clothing (a sleeping thread carries its wake deadline in
// sigdata[SIGSLEEP]). That is inherited from os32 and is NOT re-litigated
// here; it is named so the next reader knows the difference between a signal
// somebody can send and a marker the scheduler sets on itself.
typedef enum esignals
{
    // POSIX numbers, for the signals POSIX has.
    SIGHUP    = 1,
    SIGINT    = 2,
    SIGKILL   = 9,
    SIGSEGV   = 11,
    SIGPIPE   = 13,
    SIGTERM   = 15,
    SIGCONT   = 18,
    SIGSTOP   = 19,
    // 28 is SIGWINCH everywhere, and it is RESERVED here rather than defined:
    // the terminal-resize slice (DEBTS) is what gives it something to mean,
    // and a number claimed early is one nobody has to renegotiate later.
    SIGIO     = 29,   // POSIX's SIGIO/SIGPOLL

    // os64's own, in numbers POSIX left free. The scheduler markers.
    SIGHALT     = 24,
    SIGSLEEP    = 25,
    SIGUSLEEP   = 26,
    SIGLOGFLUSH = 27,
		// Raised on a task that writes to a pipe whose readers have ALL closed
		// — it is producing into the void. DEFAULT ACTION: TERMINATE, and that
		// default is load-bearing: it is the entire reason `yes | head -1`
		// exits instead of spinning forever. `head` closes its end, the writer
		// upstream gets SIGPIPE, and a program that never handles it simply
		// dies — which is exactly the right outcome. A program that wants to
		// survive a vanishing reader handles the signal explicitly.
		// (SIGPIPE = 13, above.)

		// ── The two "your world is ending" signals (2026-08-21) ─────────────
		// TWO signals, not one, because the honest answers differ. Both default
		// to death; both are catchable in principle (see the ring-3 delivery
		// debt) — and the day an app CAN catch them, the difference is what
		// lets it answer each correctly.
		//
		// SIGHUP: the terminal you were launched from is gone. The name is
		// literal — a modem dropping carrier on a dial-up line — and it is
		// the mechanism people mistake for "parent death kills children".
		// Parent death has never killed anything in Unix (orphans are
		// reparented and run on; that is what makes a daemon possible). THIS
		// is what ends a shell's leftovers, and `nohup` (PWB, 1979) exists
		// solely to opt out of it. Raised by tty_shell_departed on every task
		// still seated on the departing shell's terminal. (SIGHUP = 1, above.)

		// SIGTERM: the machine is going down; finish up. Raised by the
		// shutdown descent's termination ladder on every user task, with a
		// grace period before SIGKILL follows. A window-owning app may one
		// day reasonably survive a HUP — it has its own glass — but nothing
		// survives this one. (SIGTERM = 15, above.)

    // One past the highest number, and the size of every per-signal array.
    // 32 because the pending set is a uint32_t; every number above is well
    // inside it, and a thirteenth signal has room without a second thought.
    SIGNAL_COUNT = 32
} signals;

// The bit a signal occupies in a set. Never write `1 << SIGx` by hand — this
// is the one place the mapping lives, and the whole point of the 2026-08-23
// split is that a bit is DERIVED from a number rather than being one.
#define SIG_BIT(sig)  (1u << (uint32_t)(sig))

// ── THE PENDING SET, as a type the compiler can defend ──────────────────────
//
// A struct, not a bare uint32_t, and the wrapper is the entire safety of this
// change. `sigind |= SIGINT` used to compile and mean the right thing; after
// renumbering it would still compile and mean bit 2 instead of bit 8 —
// silently, at 33 call sites. Wrapping the word makes every one of them a
// COMPILE ERROR until it is converted deliberately, which is how a change
// like this gets made without a bug going to sleep in it.
//
// The helpers take a signals NUMBER. There is no way to pass a bit by
// accident, because there is no longer a bit to pass.
typedef struct signal_set
{
    uint32_t bits;
} signal_set_t;

static inline bool sigset_has(signal_set_t s, signals sig)
{
    return (s.bits & SIG_BIT(sig)) != 0;
}

static inline void sigset_add(signal_set_t *s, signals sig)
{
    s->bits |= SIG_BIT(sig);
}

static inline void sigset_del(signal_set_t *s, signals sig)
{
    s->bits &= ~SIG_BIT(sig);
}

// "Any of these?" — for the SIGNALS_TERMINATING family, which is a mask of
// several bits rather than one signal.
static inline bool sigset_any(signal_set_t s, uint32_t mask)
{
    return (s.bits & mask) != 0;
}

static inline void sigset_clear_mask(signal_set_t *s, uint32_t mask)
{
    s->bits &= ~mask;
}

	// THE TERMINATING SIGNALS: pending bits whose DEFAULT ACTION is death.
	// Ring 3 cannot install handlers yet (the ratified userland-signal-delivery
	// debt), so the kernel enforces the default — and it does so at three
	// checkpoints that all ask the same question: "does this thread have a
	// pending terminate?" Asking it through one macro means a new terminating
	// signal is added HERE, once, instead of being forgotten at one of them.
	//   1. the dispatcher check in _syscall_dispatch (the victim's next syscall)
	//   2. the blocking-call sentinels (console_read, pipe_read, pipe_write)
	//   3. the forced-syscall push in scheduler_run_new_thread (a spin loop)
	// (SIGPIPE also terminates, but it is raised BY the dying task inside its
	// own write() and dies on the spot — it never needs to be noticed later.
	// TRUE OF THE CHECKPOINTS, AND ONLY OF THEM, since SIGPIPE became
	// catchable: a HANDLED SIGPIPE is published as a pending bit and armed
	// like any other, so "what if delivery fails?" has a different answer
	// from "should a checkpoint kill for this?". That second question is
	// SIGNALS_DEFAULT_IS_DEATH's, below — do not reach for this mask to
	// answer it, which is exactly the mistake rd9 caught.)
	#define SIGNALS_TERMINATING  (SIG_BIT(SIGINT) | SIG_BIT(SIGKILL) | \
	                              SIG_BIT(SIGHUP) | SIG_BIT(SIGTERM))

	// The exit status a terminating signal leaves behind: the classic 128+signo
	// encoding, joining segfault's 139. Written where the signal is enforced.
	// The NUMBERS are POSIX's even though the BITS are not — a corpse tagged
	// 143 is legible to anyone who has ever read a shell's exit status, and
	// that legibility is worth keeping (DIVERGENCES § kept on merit).
	#define SIGNALS_EXIT_SIGHUP   129   // 128 + 1
	#define SIGNALS_EXIT_SIGINT   130   // 128 + 2
	#define SIGNALS_EXIT_SIGKILL  137   // 128 + 9
	#define SIGNALS_EXIT_SIGPIPE  141   // 128 + 13
	#define SIGNALS_EXIT_SIGTERM  143   // 128 + 15

	// Every signal whose DEFAULT ACTION IS DEATH — which is NOT the same
	// question as SIGNALS_TERMINATING above, and conflating them cost a real
	// bug (Codex #29 rd9). SIGNALS_TERMINATING is the set the CHECKPOINTS
	// scan: "is a pending bit here reason to stop this thread on its way past
	// me?" SIGPIPE is excluded from it on purpose, for the reason its comment
	// gives — it used to be raised by the dying task inside its own write()
	// and die on the spot, so no checkpoint ever needed to notice it.
	//
	// That stopped being the whole truth the day SIGPIPE became CATCHABLE
	// (rd1): a pending SIGPIPE now exists whenever a handler is installed and
	// delivery is armed. So when a delivery FAILS — an unusable stack, §9's
	// honest limit — the question is no longer "would a checkpoint kill for
	// this?" but "what happens when the handler cannot run?", and the answer
	// for SIGPIPE is death, exit 141. Asking the checkpoint set that question
	// let a process survive a SIGPIPE purely because its handler could not be
	// delivered.
	//
	// Two sets, two questions, and each named for the question it answers.
	#define SIGNALS_DEFAULT_IS_DEATH  (SIGNALS_TERMINATING | SIG_BIT(SIGPIPE))

	// ── PER-THREAD signal state ─────────────────────────────────────────────
	//
	// The old CAUTION that lived here — "sighandler[] and sigdata[] are indexed
	// by the signal's BIT VALUE, not its bit NUMBER" — is retired, because the
	// indexing scheme it warned about is gone. Both arrays are indexed BY
	// NUMBER now, and SIGNAL_COUNT is their size.
	//
	// WHAT IS PER-THREAD AND WHAT IS NOT (SIGNALS.md's three-way split):
	//   sigind   PER-THREAD. A thread parks and wakes on its own pending set,
	//            and a fault-derived signal is the faulting thread's business.
	//   sigdata  PER-THREAD, and it must be: sigdata[SIGSLEEP] is this
	//            thread's wake deadline, which every thread has its own of.
	//            (It is not a handler; it did not move.)
	//   sigmask  PER-THREAD, reserved for the blocked set — §7 of SIGNALS.md
	//            blocks a signal for the duration of its own handler.
	// The HANDLER table is per-TASK and lives in task_t, because the aim is
	// already a broadcast: per-thread handlers would run one SIGTERM once per
	// thread. See task.h.
	typedef struct ssignal
	{
		uint64_t     sigdata[SIGNAL_COUNT];
		signal_set_t sigmask;
		signal_set_t sigind;
	} signals_t;

	extern bool kProcessSignals;
	extern uint8_t signalProcTickFrequency;
	// RAISE a signal on a thread (NULL = the caller's own), with data — for
	// SIGSLEEP, the tick to wake at, which is what 21 of its 22 callers want.
	// Called `sigaction` until 2026-08-23, when it turned out to be holding
	// that name without setting any action; see its definition for the story.
	void signal_raise(signals signal, uint64_t sigData, void *thread);

	// ── DELIVERY (SIGNALS.md §5) ────────────────────────────────────────────
	//
	// The frame the kernel writes on the user stack before running a handler,
	// and that sigreturn restores from.
	//
	// FOUR SAVED VALUES, and the shortness is the payoff of delivering at a
	// SYSCALL BOUNDARY rather than from an interrupt. os32 delivered from the
	// scheduler ISR, so the interrupted context was an arbitrary instruction
	// and every register had to be carried. Here the interrupted context is a
	// syscall RETURN: the syscall ABI already declares RCX and R11 clobbered,
	// the entry stub preserves the callee-saved set across the dispatcher, and
	// a handler that obeys the C ABI preserves those itself. What is left that
	// nothing else will restore is the syscall's own return value and where it
	// was returning to.
	// RSP points AT this while the handler runs, so the stub reads signo and
	// handler straight off its own stack pointer. Offsets are ABI between
	// signals.c and the stub in task_exit_asm.S — the static asserts in
	// signals.c hold them together.
	typedef struct signal_frame
	{
		uint64_t magic;     // +0   SIGNAL_FRAME_MAGIC — sigreturn refuses without it
		uint64_t rax;       // +8   the interrupted syscall's return value
		uint64_t rip;       // +16  where it was returning to
		uint64_t rsp;       // +24  the stack it was returning on
		uint64_t rflags;    // +32
		uint64_t signo;     // +40  read by the stub into RDI; unblocked on return
		uint64_t handler;   // +48  read by the stub, then CALLed
		uint64_t pad;       // +56  keeps the frame 16-byte aligned, which is
		                    //      what SysV promises a called function
	} signal_frame_t;

	// Not a hash, just an unlikely constant: it turns "ring 3 handed us a
	// pointer to anything at all" into a refusal rather than a register load.
	// The RFLAGS sanitization and running-handler checks are the real defences
	// (see syscall_sigreturn); this catches the honest mistakes first and
	// cheapest.
	#define SIGNAL_FRAME_MAGIC 0x5349475246524D45ULL   /* "SIGRFRME" */

	// ── THE FULL FRAME (SIGNALS.md §10) — scheduler delivery to a spinner ───
	//
	// A thread interrupted at an ARBITRARY ring-3 instruction (a spin loop the
	// timer caught, not a syscall return) has every register live, so the
	// frame carries the whole file. It BEGINS with signal_frame_t byte-for-
	// byte — magic, rax, rip, rsp, rflags, signo at +40, handler at +48 — so
	// the ONE stub in task_exit_asm.S serves both delivery paths unchanged.
	//
	// A SECOND MAGIC, not a flag field, tells sigreturn which restore it is
	// being asked for: a flag inside user-writable memory would be ring 3's
	// to flip, upgrading a 4-value restore into a full-file restore. Two
	// magics mean forging one buys only that frame kind's own validation.
	//
	// NO FXSAVE AREA, BY CONSTRUCTION: userland is built -mno-mmx -mno-sse
	// -mno-sse2 (userland/GNUmakefile), so there is no vector state to be
	// live at the interruption point. If those flags ever change, this frame
	// grows a 512-byte fxsave area or float code corrupts across delivery.
	//
	// Segment selectors are deliberately NOT in the frame: sigreturn restores
	// them from GDT constants. A selector a program can write is a selector a
	// program can forge, and there is exactly one correct answer anyway.
	typedef struct signal_frame_full
	{
		signal_frame_t base;    // +0..+63, the §5 frame, same stub offsets
		uint64_t rbx;           // +64
		uint64_t rcx;           // +72
		uint64_t rdx;           // +80
		uint64_t rsi;           // +88
		uint64_t rdi;           // +96
		uint64_t rbp;           // +104
		uint64_t r8;            // +112
		uint64_t r9;            // +120
		uint64_t r10;           // +128
		uint64_t r11;           // +136
		uint64_t r12;           // +144
		uint64_t r13;           // +152
		uint64_t r14;           // +160
		uint64_t r15;           // +168
	} signal_frame_full_t;      // 176 bytes — keeps RSP 16-aligned

	#define SIGNAL_FRAME_MAGIC_FULL 0x5349475246524D32ULL   /* "SIGRFRM2" */

	// What sigreturn lets a frame say about RFLAGS. The frame sits on the
	// USER'S OWN WRITABLE STACK, so its rflags word is ring 3's to forge no
	// matter who wrote it first — and sysretq loads RFLAGS from R11 nearly
	// verbatim, including IF and IOPL. A forged IF=0 parks a core outside the
	// timer's reach forever (only the NMI probe would ever see it again); a
	// forged IOPL=3 hands ring 3 the I/O ports and cli. So sigreturn KEEPS
	// only the bits a user program legitimately owns — the arithmetic flags,
	// TF, DF, AC, ID — and FORCES the rest: IF on, IOPL 0, bit 1 (the
	// always-one reserved bit).
	//
	// §10's full-frame sigreturn must inherit this discipline with higher
	// stakes: its road home is iretq, which swallows RFLAGS whole and CS/SS
	// besides. Flags pass through this same mask; selectors come from kernel
	// constants, never from the frame.
	#define SIGNAL_RFLAGS_USER_BITS 0x240DD5ULL /* CF PF AF ZF SF TF DF OF AC ID */
	#define SIGNAL_RFLAGS_FORCED    0x202ULL    /* IF=1, reserved bit 1 = 1 */

	// The DIRECTION FLAG, cleared on the way INTO a handler and nowhere else
	// (Codex #29 rd10). The SysV ABI requires DF clear at every function
	// entry, and a signal handler is an ordinary C function the kernel calls
	// out of nowhere — so if an async signal interrupts ring-3 code with DF
	// set, the handler inherits it and any string operation the compiler
	// emits inside it (a struct copy, a memcpy, a printf) runs BACKWARD.
	//
	// Note that DF stays in SIGNAL_RFLAGS_USER_BITS above, and must: the
	// frame's saved copy carries the INTERRUPTED flags, and sigreturn has to
	// put the interrupted code back exactly as it was, DF included. Only the
	// LIVE rflags — the ones the CPU will be running the handler on — get
	// this cleared. Save the original, enter clean, restore the original.
	//
	// Honest about the reach: no os64 program sets DF today (there is not one
	// `std` in userland), and no compiler leaves it set across a call. This is
	// the same bargain §10 already makes for the red zone — hand-written
	// ring-3 asm is allowed to exist, and three instructions is a cheap price
	// for never having to debug a handler whose memcpy ran the wrong way.
	#define SIGNAL_RFLAGS_DF        (1ULL << 10)

	// The user-address boundary the sigreturn paths check a resume RIP/RSP
	// against lives in paging.h as USER_CANONICAL_MAX — it is an address-space
	// fact, shared with paging_resolve_user_writable and the delivery writer.

	// What signal_deliver_pending accomplished, and the dispatcher's duty for
	// each: NONE — nothing pending and handled, carry on. ARMED — a handler
	// was armed; the syscall's return value is saved in the frame and the
	// caller must not clobber it. FAILED — a handled signal is pending but the
	// frame could not be written (the stack is unusable, SIGNALS.md §9): the
	// dispatcher applies the DEFAULT ACTION instead, because leaving the bit
	// pending with a handler installed would mean every blocking call returns
	// INTERRUPTED forever while the signal neither delivers nor kills — a
	// livelock only SIGKILL ends.
	typedef enum
	{
		SIGNAL_DELIVER_NONE   = 0,
		SIGNAL_DELIVER_ARMED  = 1,
		SIGNAL_DELIVER_FAILED = 2,
	} signal_deliver_result_t;

	// Deliver one pending, handled signal to ring 3 if there is one, by
	// rewriting where the current syscall returns to. Called at the
	// dispatcher's exit, in the victim's own context.
	struct task;
	signal_deliver_result_t signal_deliver_pending(struct task *t, void *thread, uint64_t retval);

	// The §10 sibling: deliver by rewriting thread->regs — for a thread the
	// SCHEDULER caught spinning in ring 3, where regs already hold the full
	// interrupted context. Builds a signal_frame_full_t on the user stack,
	// points regs.RIP at the stub and regs.RSP at the frame. The CALLER
	// (scheduler_signal_visit) mirrors RIP/RSP into the per-core isr arrays —
	// both images, the forced push's own discipline. On FAILED nothing was
	// changed and the pending bit is untouched: a terminating signal falls
	// through to the gallows, which is exactly the design (death must not
	// depend on the victim's stack).
	signal_deliver_result_t signal_deliver_to_regs(struct task *t, void *thread);

	// SIGSEGV delivery from the PAGE-FAULT handler (SIGNALS.md §9 — the arc's
	// acceptance test). `context` is an exception_context_t* (void here to
	// keep exception_report.h out of this widely-included header). Returns
	// true if a handler was armed and the fault handler should RESUME (the
	// context now runs the stub); false if it must proceed to kill the task
	// (no handler, the handler itself faulted, or the faulted stack cannot
	// hold the frame). Synchronous and thread-local — see the definition.
	bool signal_deliver_segv(struct task *t, void *thread, void *context);

	// Is there a pending signal something will CATCH? The default-action
	// checkpoints ask before they kill — a task that installed a handler must
	// not be executed on its way to being handed the signal it asked for.
	// Always false for SIGKILL, which is what keeps it the last resort.
	bool signal_has_handler_for_pending(struct task *t, void *thread);

	// Can ring 3 install a handler for this signal? Range check plus the one
	// exception (SIGKILL), in ONE place so registration and delivery can never
	// reach different conclusions about the same signal.
	//
	// Both are MEMBERSHIP tests over the public signal set, not range checks
	// (rd13): the gaps in the numbering are not signals, and neither are the
	// scheduler markers at 24-27, which share the sigind word but are thread
	// STATE. A range check let ring 3 register a handler on SIGSLEEP.
	//
	// TWO predicates because the ABI has two refusals and they are not the
	// same sentence: BAD_SIGNAL means "no such signal", UNCATCHABLE means
	// "SIGKILL". Everything known is catchable except SIGKILL.
	bool signal_is_known(signals sig);
	bool signal_is_catchable(signals sig);

	// Install a handler on the TASK and return the one it replaced. NULL
	// restores the kernel default. (struct task, not task_t: signals.h is
	// included BY task.h, so it cannot see the full type.)
	struct task;
	void *signal_set_handler(struct task *t, signals sig, void *handler);
	void init_signals();
	
#endif
