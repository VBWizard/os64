// console.c — the blocking stdin read abstraction (the TTY seed). See
// console.h for the layering. keyboard.c is the device; this owns the sleeping
// reader and the wake discipline; the read syscall bridges it to ring 3.

#include "console.h"
#include "driver/system/keyboard.h"
#include "scheduler.h"
#include "signals.h"
#include "smp_core.h"
#include "thread.h"
#include "task.h"
#include "kernel.h"
#include "CONFIG.h"

extern volatile uint64_t kTicksSinceStart;

// The single thread currently blocked in console_read (NULL = none). One slot
// because v1 has one console; this becomes a per-tty_t field when TTYs arrive
// and each shell reads its own keyboard stream.
static thread_t * volatile kConsoleWaiter = NULL;

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

// EOT can arrive after bytes have already been gathered in the same drain
// ("abc<Ctrl+D>"). Terminal semantics: deliver the bytes NOW, deliver the EOF
// on the NEXT read. One console in v1, so one flag; becomes per-tty_t later
// alongside kConsoleWaiter. The EOF is one-shot — consuming it re-arms the
// console for normal reading (Ctrl+D ends cat, then husk's prompt reads on).
static volatile bool kConsoleEOFPending = false;

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
bool console_intr_intercept(char ascii)
{
	if (ascii != CONSOLE_ETX)
		return false;

	task_t *fg = kForegroundTask;
	if (fg == NULL || fg->controllingShell)
		return false;               // no owner yet, or the shell: stays data

	thread_t *t = fg->threads;
	if (t == NULL)
		return false;

	// One word-OR — all an IRQ path is allowed to do. The victim dies at its
	// own next syscall boundary (dispatcher check / blocking-loop checks in
	// console_read and pipe.c), in its own context, through the normal
	// task_exit path. If it is parked in ISLEEP, processSignals sees this bit
	// and wakes it into that check within a scheduler pass (~10ms).
	t->signals.sigind |= SIGINT;
	return true;                    // consumed: the byte never enters the ring
}

long console_read(char *buf, size_t len)
{
	if (len == 0)
		return 0;

	// An EOT from a previous drain is a promised EOF — deliver it first.
	if (kConsoleEOFPending)
	{
		kConsoleEOFPending = false;
		return 0;
	}

	core_local_storage_t *cls = get_core_local_storage();
	thread_t *self = cls->currentThread;

	// A BACKGROUND job gets EOF instead of the keyboard — `cmd &` behaves as
	// `cmd < /dev/null &`. Checked before the loop, not inside it: a background
	// reader must never join the waiter queue at all, or it would take
	// keystrokes the shell was owed. os32 had this hole and never noticed,
	// because nothing anyone backgrounded there happened to read stdin.
	//
	// Keyed on backgroundJob (task.h) and NOT on kForegroundTask, deliberately:
	// in `cat | upper` husk waits on the LAST stage, so `cat` is not the
	// foreground task, and gating on that would hand the first stage of every
	// console-reading pipeline an instant EOF.
	//
	// Writes stay untouched — a background job still prints to the screen,
	// which was always the useful half of the bargain.
	if (cls->task != NULL && cls->task->backgroundJob)
		return 0;

	size_t n = 0;
	for (;;)
	{
		// A pending TERMINATE outranks the read: the READER is being killed,
		// whether by Ctrl+C or by a write to its /proc ctl file. Checked at the
		// top of every pass — this is how a reader parked below (and woken by
		// processSignals when the bit appeared) exits the loop instead of
		// parking forever. Any bytes in the ring stay for the next reader; a
		// dying task has no further use for them.
		if (self->signals.sigind & SIGNALS_TERMINATING)
			return CONSOLE_READ_INTERRUPTED;

		// Drain whatever translated keys are queued (skip pure-modifier /
		// non-glyph events — they have ascii == 0).
		keyboard_event_t ev;
		while (n < len && keyboard_pop_event(&ev))
		{
			if (ev.ascii == CONSOLE_EOT)
			{
				if (n == 0)
					return 0;             // EOF right now: nothing precedes it
				kConsoleEOFPending = true; // bytes first, EOF on the next read
				break;
			}
			if (ev.ascii)
				buf[n++] = ev.ascii;
		}

		if (n > 0)
			return (long)n;   // got input — return it (terminal semantics)

		// Nothing available: register as the waiter and sleep. SIGSLEEP parks
		// us atomically (the scheduler performs RUNNING->ISLEEP when we are
		// genuinely off-CPU, so there is no "runnable while still executing"
		// window). We resume here when woken — by a keypress via
		// console_wake_if_ready, by processSignals on a pending SIGINT, or by
		// the backstop — and loop back to the SIGINT check and the drain.
		kConsoleWaiter = self;
		sigaction(SIGSLEEP, NULL, kTicksSinceStart + CONSOLE_READ_BACKSTOP_TICKS, self);
	}
}

void console_wake_if_ready(void)
{
	thread_t *w = kConsoleWaiter;
	// Only wake a reader that is actually parked (ISLEEP). If it is still
	// RUNNING (mid-registration), leave the waiter set and catch it next pass
	// once it has parked — this is what makes the path lost-wakeup-free.
	if (w != NULL && w->threadState == THREAD_STATE_ISLEEP && keyboard_has_event())
	{
		kConsoleWaiter = NULL;
		w->signals.sigind &= ~SIGSLEEP;     // cancel the backstop sleep
		w->signals.sigdata[SIGSLEEP] = 0;
		// _locked: our only caller is processSignals, which holds the
		// scheduler queue lock across this wake (that's what makes the
		// ISLEEP check above trustworthy). The public variant would
		// re-acquire the lock and self-deadlock.
		scheduler_change_thread_queue_locked(w, THREAD_STATE_RUNNABLE);
	}
}
