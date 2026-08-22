#ifndef SIGNALS_H
#define SIGNALS_H

#include <stdint.h>
#include <stdbool.h>

  typedef struct ssignal
    {
        void* sighandler[32];
        uint64_t sigdata[32];
        uint32_t sigmask;
        uintptr_t sigind;
        
    } signals_t;

    typedef enum esignals
    {
        SIGHALT = 1,
        SIGSLEEP = 1 << 1,
        SIGUSLEEP = 1 << 2,
        SIGINT = 1 << 3,
        SIGSEGV = 1 << 4,
        SIGSTOP = 1 << 5,
        SIGIO = 1 << 6,
        // These are BIT VALUES, not signal numbers: sigind is a bitmask and
        // every entry here must be a distinct single bit. SIGKILL was `9` —
        // inherited from os32, where it meant POSIX signal number 9 — which in
        // a bitmask reads as SIGHALT|SIGINT (1|8). Nothing raised it, so the
        // lie stayed harmless for years; the moment /proc's ctl file wanted to
        // send it (PROC.md), `sigind |= SIGKILL` would have silently raised a
        // keyboard interrupt on a task nobody touched. Bit 8 was free.
        SIGKILL = 1 << 8,
        SIGCONT = 1 << 7,
		SIGLOGFLUSH = 1 << 9,
		// Raised on a task that writes to a pipe whose readers have ALL closed
		// — it is producing into the void. DEFAULT ACTION: TERMINATE, and that
		// default is load-bearing: it is the entire reason `yes | head -1`
		// exits instead of spinning forever. `head` closes its end, the writer
		// upstream gets SIGPIPE, and a program that never handles it simply
		// dies — which is exactly the right outcome. A program that wants to
		// survive a vanishing reader handles the signal explicitly.
		SIGPIPE = 1 << 10,

		// ── The two "your world is ending" signals (2026-08-21) ─────────────
		// TWO bits, not one, because the honest answers differ. Both default
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
		// still seated on the departing shell's terminal.
		SIGHUP = 1 << 11,

		// SIGTERM: the machine is going down; finish up. Raised by the
		// shutdown descent's termination ladder on every user task, with a
		// grace period before SIGKILL follows. A window-owning app may one
		// day reasonably survive a HUP — it has its own glass — but nothing
		// survives this one.
		SIGTERM = 1 << 12
    } signals;

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
	// own write() and dies on the spot — it never needs to be noticed later.)
	#define SIGNALS_TERMINATING  (SIGINT | SIGKILL | SIGHUP | SIGTERM)

	// The exit status a terminating signal leaves behind: the classic 128+signo
	// encoding, joining segfault's 139. Written where the signal is enforced.
	// The NUMBERS are POSIX's even though the BITS are not — a corpse tagged
	// 143 is legible to anyone who has ever read a shell's exit status, and
	// that legibility is worth keeping (DIVERGENCES § kept on merit).
	#define SIGNALS_EXIT_SIGHUP   129   // 128 + 1
	#define SIGNALS_EXIT_SIGINT   130   // 128 + 2
	#define SIGNALS_EXIT_SIGKILL  137   // 128 + 9
	#define SIGNALS_EXIT_SIGTERM  143   // 128 + 15

	// CAUTION, sighandler[] / sigdata[]: both are indexed by the signal's BIT
	// VALUE, not its bit NUMBER — sigdata[SIGSLEEP] is sigdata[2]. That works
	// only for bits 0..4; SIGSTOP (32) and everything above it would index past
	// the end of a 32-entry array. Nothing does today (only SIGSLEEP and SIGINT
	// are ever used as indices), but any new code that wants per-signal data
	// must fix the indexing scheme first rather than add another landmine.

	extern bool kProcessSignals;
	extern uint8_t signalProcTickFrequency;
	void *sigaction(int signal, uintptr_t *sigAction, uint64_t sigData, void *thread);
	void init_signals();
	
#endif
