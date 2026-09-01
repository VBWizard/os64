// console.c — the blocking stdin read abstraction (the TTY seed — and since
// 2026-08-08, the TTY's read discipline, multiplied by eight). See console.h
// for the layering. keyboard.c/xhci.c are the devices; tty.c owns the rings
// and grids; THIS file owns the sleeping reader and the wake discipline, one
// per terminal now; the read syscall bridges it to ring 3.
//
// THE MULTIPLICATION (each singleton's doctrine is unchanged — the comments
// moved to tty.h with the fields): kConsoleWaiter, kConsoleEOFPending, and
// kConsolePushback were born with comments promising to become per-tty_t
// fields when virtual terminals arrived. They kept the promise. A reader now
// reads ITS OWN terminal (task_tty — the controlling terminal, inherited at
// creation), so husk on tty2 sleeps on tty2's ring while husk on tty1 echoes,
// and neither can eat a keystroke the other was owed.

#include "console.h"
#include "tty.h"
#include "driver/system/keyboard.h"
#include "BasicRenderer.h"   // renderer_cursor_show/hide — the listening light
#include "scheduler.h"
#include "signals.h"
#include "smp_core.h"
#include "thread.h"
#include "task.h"
#include "kernel.h"
#include "CONFIG.h"

extern volatile uint64_t kTicksSinceStart;

// ASCII EOT, "End of Transmission" — what the keyboard driver emits for
// Ctrl+D (Ctrl+letter strips the letter to its 1963 control code). The console
// turns it into end-of-input: read() returns 0, which the read syscall already
// relays as EOF exactly like a drained pipe or a file at its end. This is the
// same reason Unix chose Ctrl+D — EOT already MEANT this before either OS.
#define CONSOLE_EOT 0x04

// ASCII ETX, "End of Text" — what Ctrl+C strips down to, and the terminal
// interrupt character since the DEC line disciplines. Same 1963 well EOT
// drinks from: Unix's ISIG/VINTR machinery is what turned this keystroke
// into a signal, and that is the lineage being honored here, not imitated.
#define CONSOLE_ETX 0x03

// The unread slot's depth (the slot itself lives in tty_t now; console.h has
// the origin story). Four is generous — the only caller holds one byte at a
// time, and each terminal gets its own four.
#define CONSOLE_PUSHBACK_MAX 4

bool console_unread(char c)
{
	core_local_storage_t *cls = get_core_local_storage();
	tty_t *tty = task_tty(cls ? cls->task : NULL);
	if (tty->pushbackCount >= CONSOLE_PUSHBACK_MAX)
		return false;
	tty->pushback[tty->pushbackCount++] = c;
	return true;
}

// How long the reader sleeps before waking to re-check, as a BACKSTOP only.
// A keypress normally wakes it far sooner via console_wake_if_ready (next
// scheduler pass, ~10ms). The backstop just guarantees liveness — if a wake
// were ever missed, the reader still re-checks within a second (one idle wake
// per second, negligible). NOT a polling interval: while idle and untyped, the
// reader is genuinely asleep between these.
#define CONSOLE_READ_BACKSTOP_TICKS TICKS_PER_SECOND

// The interrupt-character policy (see console.h). Why this is a SIGNAL and
// not a console byte: the classic victim (cat writing a huge file) is not
// READING the console — nothing drains the ring, so an 0x03 buffered there
// would sit unread forever. The interrupt must be delivered asynchronously,
// at the keystroke. And why the SHELL gets the byte instead: Ctrl+C at an
// idle prompt must never kill your shell — husk treats a data-byte 0x03 as
// line-kill (echo ^C, fresh prompt), so the key always visibly DOES something.
//
// PER-TTY NOW: the keystroke happened on the FOCUSED terminal, so the victim
// is the FOCUSED terminal's foreground task — a compile grinding away on
// tty3 is perfectly safe from the Ctrl+C you type at tty1's prompt. (This is
// what "the console belongs to somebody" always meant; there are just eight
// consoles now, exactly as SIGINT.md prescribed.)
// The tty-scoped core (split 2026-08-19 for ptys, PTY.md): a pty master's
// write runs this against the SLAVE — the keystroke "happened" on the
// terminal that window represents, so the victim is THAT terminal's
// foreground, exactly as a VT's Ctrl+C aims at the VT's own. The policy
// below is unchanged; only who asks moved.
bool console_intr_intercept_tty(tty_t *tty, char ascii)
{
	if (ascii != CONSOLE_ETX)
		return false;

	if (tty == NULL)
		tty = &kTTY[0];

	task_t *fg = tty->fgTask;
	if (fg == NULL || fg->controllingShell)
		return false;               // no owner yet, or the shell: stays data

	if (fg->threads == NULL)
		return false;

	// A word-OR per thread — still all an IRQ path is allowed to do, and a
	// read-only walk of a chain whose nodes are published fully linked.
	// EVERY thread, because Ctrl+C means "stop that program", and a
	// program is now allowed to be more than one thread: signalling only
	// the first left workers running after their parent had died (the
	// `ctl kill` version of this bug was audible as fan noise, 2026-08-02).
	//
	// Each victim dies at its own next syscall boundary (dispatcher check /
	// blocking-loop checks in console_read and pipe.c), in its own context,
	// through the normal task_exit path. Any parked in ISLEEP are woken
	// into that check by processSignals within a scheduler pass (~10ms).
	task_signal_all_threads(fg, SIGINT);
	return true;                    // consumed: the byte never enters the ring
}

// The keyboard's spelling: the keystroke happened on the FOCUSED terminal.
bool console_intr_intercept(char ascii)
{
	return console_intr_intercept_tty(kTTYFocused, ascii);
}

long console_read(char *buf, size_t len)
{
	// The classic blocking read is the no-deadline spelling of the timed one.
	return console_read_deadline(buf, len, 0);
}

long console_read_deadline(char *buf, size_t len, uint64_t deadline)
{
	if (len == 0)
		return 0;

	core_local_storage_t *cls = get_core_local_storage();
	thread_t *self = cls->currentThread;
	// The terminal this reader answers to. NULL-safe all the way down —
	// early-boot probes and kernel threads read the system console, VT1.
	tty_t *tty = task_tty(cls->task);

	// An EOT from a previous drain is a promised EOF — deliver it first.
	if (tty->eofPending)
	{
		tty->eofPending = false;
		return 0;
	}

	// A BACKGROUND job gets EOF instead of the keyboard — `cmd &` behaves as
	// `cmd < /dev/null &`. Checked before the loop, not inside it: a background
	// reader must never join the waiter queue at all, or it would take
	// keystrokes the shell was owed. os32 had this hole and never noticed,
	// because nothing anyone backgrounded there happened to read stdin.
	//
	// Keyed on backgroundJob (task.h) and NOT on the foreground pointer,
	// deliberately: in `cat | upper` husk waits on the LAST stage, so `cat`
	// is not the foreground task, and gating on that would hand the first
	// stage of every console-reading pipeline an instant EOF.
	//
	// Writes stay untouched — a background job still prints to the screen,
	// which was always the useful half of the bargain.
	if (cls->task != NULL && cls->task->backgroundJob)
		return 0;

	// The listening light is only honest on the terminal the glass is
	// showing: a reader parked on a background terminal must never light
	// (or douse) the cursor the FOCUSED terminal's reader owns. Focus can
	// move while we sleep; re-checked at every touch.

	size_t n = 0;
	for (;;)
	{
		// A pending TERMINATE outranks the read: the READER is being killed,
		// whether by Ctrl+C or by a write to its /proc ctl file. So does a
		// pending signal the reader will CATCH — SIGWINCH on a shell parked at
		// its prompt is the everyday case — because the handler can only be
		// armed once the read returns (signal_park_must_end). Checked at the
		// top of every pass — this is how a reader parked below (and woken by
		// processSignals when the bit appeared) exits the loop instead of
		// parking forever. Any bytes in the ring stay for the next reader; a
		// dying task has no further use for them, and an interrupted one
		// comes back for them.
		if (signal_park_must_end(self))
		{
			// Un-register on the way out (here and at every exit below): a
			// reader that leaves the loop while the waiter slot still names it
			// can be spuriously woken out of some LATER unrelated sleep when
			// a key arrives. The blocking read never met this — it only left
			// with bytes or died — but the deadline path returns alive and
			// empty-handed, which made the stale slot a live bug to have.
			if (tty->waiter == self)
				tty->waiter = NULL;
			if (kTTYFocused == tty)
				renderer_cursor_hide();
			return CONSOLE_READ_INTERRUPTED;
		}

		// Pushed-back bytes first — console_unread's contract (console.h):
		// what a probe returned must reach the next reader ahead of the ring.
		while (n < len && tty->pushbackCount > 0)
			buf[n++] = tty->pushback[--tty->pushbackCount];

		// Drain whatever translated keys are queued on OUR terminal (skip
		// pure-modifier / non-glyph events — they have ascii == 0).
		keyboard_event_t ev;
		while (n < len && tty_input_pop(tty, &ev))
		{
			if (ev.ascii == CONSOLE_EOT)
			{
				if (n == 0)
				{
					if (kTTYFocused == tty)
						renderer_cursor_hide();
					return 0;             // EOF right now: nothing precedes it
				}
				tty->eofPending = true;   // bytes first, EOF on the next read
				break;
			}
			if (ev.ascii)
				buf[n++] = ev.ascii;
		}

		if (n > 0)
		{
			if (tty->waiter == self)
				tty->waiter = NULL;
			// Hide before the caller echoes: the print path would hide it
			// anyway, but a caller that DOESN'T echo must not leave a lit
			// cursor claiming the machine is still listening.
			if (kTTYFocused == tty)
				renderer_cursor_hide();
			return (long)n;   // got input — return it (terminal semantics)
		}

		// THE LINE IS DEAD. A pty whose master has closed can never receive
		// another byte, so an empty read on one is EOF — the same answer a
		// hung-up terminal has given since the modem era, and the reason
		// masterClosed is checked HERE and not only announced as SIGHUP.
		// The signal alone was not enough: it is consumed on delivery, so a
		// program that CATCHES SIGHUP (rather than dying of it) came back
		// from its handler, called read again, and parked forever on a line
		// nobody held — keeping its seat, and keeping the pty from ever
		// being buried, which is the orphan the hangup exists to prevent.
		// After the drain, so whatever the master typed before it left is
		// delivered first: bytes, then EOF.
		if (tty->is_pty && tty->masterClosed)
		{
			if (tty->waiter == self)
				tty->waiter = NULL;
			if (kTTYFocused == tty)
				renderer_cursor_hide();
			return 0;
		}

		// Empty-handed and out of patience: the deadline verdict. Checked
		// AFTER the drain, so a poll (deadline already past) still delivers
		// anything that was waiting — the deadline caps the WAIT, never the
		// read. >= makes a deadline of "now" the poll gait by construction.
		if (deadline != 0 && kTicksSinceStart >= deadline)
		{
			if (tty->waiter == self)
				tty->waiter = NULL;
			if (kTTYFocused == tty)
				renderer_cursor_hide();
			return CONSOLE_READ_TIMEOUT;
		}

		// Nothing available: register as OUR terminal's waiter and sleep.
		// SIGSLEEP parks us atomically (the scheduler performs
		// RUNNING->ISLEEP when we are genuinely off-CPU, so there is no
		// "runnable while still executing" window). We resume here when
		// woken — by a keypress via console_wake_if_ready, by processSignals
		// on a pending signal this park must end for (a terminate, or one a
		// handler will catch), or by the backstop — and loop back to the
		// signal_park_must_end check and the drain. A live deadline shortens the backstop
		// nap so the timeout verdict lands on time, not up to a second late.
		uint64_t wake = kTicksSinceStart + CONSOLE_READ_BACKSTOP_TICKS;
		if (deadline != 0 && deadline < wake)
			wake = deadline;
		tty->waiter = self;
		// About to park empty-handed: light the cursor — but only if OUR
		// terminal is the one on the glass. It sits at wherever the caller's
		// last echo left the console cursor — mid-line during husk's
		// editing, end of prompt otherwise — and every output path (and
		// every exit above) puts it away. Show is idempotent, so the
		// backstop re-loop costs a repaint at worst. A reader parked on a
		// background terminal gets its light when the terminal gets the
		// glass (tty_focus relights it from the repaint).
		if (kTTYFocused == tty)
			renderer_cursor_show();
		signal_raise(SIGSLEEP, wake, self);
	}
}

void console_wake_if_ready(void)
{
	// One sweep over the fleet: wake any terminal's reader whose ring has
	// input. Only the FOCUSED terminal ever receives new keystrokes, but a
	// reader may have parked on a terminal that had type-ahead queued, and
	// the backstop discipline is per-terminal — the sweep stays level-
	// triggered on the CONDITION (ring non-empty), which is what keeps every
	// one of these paths lost-wakeup-free.
	for (uint32_t i = 0; i < TTY_COUNT; i++)
	{
		tty_t *t = &kTTY[i];
		thread_t *w = t->waiter;
		// Only wake a reader that is actually parked (ISLEEP). If it is
		// still RUNNING (mid-registration), leave the waiter set and catch
		// it next pass once it has parked.
		if (w != NULL && w->threadState == THREAD_STATE_ISLEEP && tty_input_has(t))
		{
			t->waiter = NULL;
			sigset_del(&w->signals.sigind, SIGSLEEP);     // cancel the backstop sleep
			w->signals.sigdata[SIGSLEEP] = 0;
			// _locked: our only caller is processSignals, which holds the
			// scheduler queue lock across this wake (that's what makes the
			// ISLEEP check above trustworthy). The public variant would
			// re-acquire the lock and self-deadlock.
			scheduler_change_thread_queue_locked(w, THREAD_STATE_RUNNABLE);
		}
	}

	// The pty leg (PTY.md): a reader parked on a slave must wake on the
	// master's injected input, not on its one-second backstop — found in
	// design review before it could be a bug. Lives in tty.c because the
	// walk must hold the registry lock (a node mid-walk could otherwise be
	// buried under our feet — a parked reader's own slave is seat-pinned,
	// but the nodes we walk THROUGH to reach it are not).
	tty_pty_wake_readers();
}
