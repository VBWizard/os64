#ifndef OS64_SIGNAL_H
#define OS64_SIGNAL_H

#include <stdint.h>
#include "os64/syscall.h"
#include "os64/syscall_numbers.h"

// os64/signal.h — the signals a program can be told about, and how to ask.
//
// THE NUMBERS ARE THE ABI. They live here rather than in the kernel's
// signals.h because ring 3 cannot include kernel headers, and the kernel
// STATIC-ASSERTS its own enum against this file (signals.c) — so renumbering
// one without the other stops the build instead of quietly delivering the
// wrong signal to the wrong handler. Same discipline as klog_format.h's
// category table, and for the same reason.
//
// They are POSIX's numbers where POSIX has one, kept on merit (DIVERGENCES):
// a corpse tagged 143 is legible to anyone who has ever read a shell's exit
// status. os64's own take numbers POSIX left free.

// WHAT A CORPSE IS TAGGED WITH when a signal killed it: 128 plus the number.
// Published as the RULE rather than as a list, because the rule is the
// durable half and a list would be a second place for the numbers to live.
//
// It is here for the program that CATCHES a signal and then exits itself: the
// kernel's default death writes this code, so a handler that tidies up and
// leaves must write the same one or a script cannot tell "interrupted" from
// whatever the program returns on a good day. /bin/gopher wants it because
// Ctrl+C is the only way out of a blocking call it cannot put a deadline on,
// and the terminal it painted has to go back before it dies.
#define OS64_EXIT_FOR_SIGNAL(signo)  (128 + (signo))

#define OS64_SIGHUP    1    // your terminal hung up — the seated shell died, or the pty master closed; either way the line went away
#define OS64_SIGINT    2    // Ctrl+C
#define OS64_SIGKILL   9    // uncatchable, by design — see below
#define OS64_SIGSEGV   11   // you touched memory that isn't yours
#define OS64_SIGPIPE   13   // you are writing into a pipe nobody reads
#define OS64_SIGTERM   15   // the machine is going down; finish up
// NUMBERED BUT NOT YET REAL — all three below. Nothing in the kernel raises
// any of them: job control (stop/continue semantics, husk's fg/bg, the
// debugger's `ctl stop`) is a booked slice, not a shipped one, and SIGIO was
// claimed alongside the POSIX numbers and never given a sender. So until a
// producer lands, os64_signal_set_handler REFUSES these with
// OS64_SIG_ERR_BAD_SIGNAL rather than reporting a success that leaves you
// waiting for a signal nothing can send. The numbers are claimed here so that
// day changes only behaviour, not the ABI. (Codex #29 rd14/rd15: they used to
// be accepted, which was the same contract failure as accepting a number the
// kernel does not have.)
#define OS64_SIGCONT   18
#define OS64_SIGSTOP   19
#define OS64_SIGIO     29
// Your terminal changed size. Raised by pty_resize (a terminal window that
// was dragged tells its pty) at every task seated on that terminal that has
// a handler for it. DEFAULT: IGNORED — a program that never asks is
// unaffected, and gets no pending bit at all (nothing waits for a handler
// installed later to fire for an old resize). A program that cares
// installs a handler and then RE-OPENS /proc/self/tty to read `cols` and
// `rows`: the signal carries no payload (the pending set is a bitmask, two
// resizes before delivery are one), and procfs renders a file at OPEN, so a
// handle held across the signal reports the old size forever. Expect a
// blocking call in flight to answer OS64_INTERRUPTED (that is how the handler
// gets to run) and loop — SIGNALS.md §8, no restart.
#define OS64_SIGWINCH  28

#define OS64_SIGNAL_COUNT 32

// WHAT AN INTERRUPTED CALL ANSWERS. A blocking call cut short by a signal
// this program handled at the moment it woke returns this instead of its
// normal result: the wait ended early, nothing was accomplished, and the
// caller decides what to do about it. It does NOT promise the handler ran —
// it usually has, but a sibling thread can uninstall the handler between
// the wake and the return, and the call still answers this.
//
// os64 has no SA_RESTART and no EINTR. POSIX shipped both behaviours because
// its authors could not decide, and every caller since has had to learn which
// one it got. Here a call that was interrupted says so, and a program that
// wants to retry writes a loop — which anyone reading it can see.
//
// (This can only ever happen to a program that INSTALLED a handler. A signal
// nothing catches still takes its default action, which for the terminating
// ones is death, exactly as before.)
#define OS64_INTERRUPTED (-4)

// A handler is an ordinary C function taking the signal number. It receives
// nothing else — os64 has no siginfo, and the pending set is a bitmask, so
// two SIGTERMs before delivery are one SIGTERM. That is classic Unix
// behaviour and is not a bug.
typedef void (*os64_signal_fn)(int signo);

// The handler value meaning "do whatever the kernel would have done".
#define OS64_SIG_DEFAULT ((os64_signal_fn)0)

// Errors, negative and distinct so a caller can tell WHY it was refused.
#define OS64_SIG_ERR_BAD_SIGNAL   (-1)   // not a signal number this kernel knows
#define OS64_SIG_ERR_UNCATCHABLE  (-2)   // SIGKILL; see below
#define OS64_SIG_ERR_BAD_HANDLER  (-3)   // not an address ring 3 could run

// Install `handler` for `signo`, and return the handler it replaced (which may
// be OS64_SIG_DEFAULT). Pass OS64_SIG_DEFAULT to go back to the kernel's own
// behaviour. Negative return = one of the errors above; nothing was changed.
//
// THE HANDLER IS THE PROGRAM'S, not the thread's. Install it once, from
// anywhere, and it covers every thread — because a signal aimed at a program
// is delivered to all of them, and a handler that ran once per thread would
// fire your "wait, I have unsaved work" four times in a four-threaded app.
// (SIGNALS.md §2 has the argument and the 2026-08-02 scar behind it.)
//
// SIGKILL IS REFUSED, loudly, with OS64_SIG_ERR_UNCATCHABLE. It is the answer
// to a program that has stopped answering, and a kernel that let a program
// decline to die would have no last resort.
//
// WHAT YOU CAN CATCH, exactly: SIGHUP, SIGINT, SIGSEGV, SIGPIPE, SIGTERM and
// SIGWINCH — the signals this kernel can actually send. SIGSEGV included, though a handler
// for it runs on the stack that just faulted, so if the STACK is what went
// wrong there is nowhere to put the frame and the thread dies as it always
// did. Every other number — the gaps, and the NUMBERED-BUT-NOT-YET-REAL
// three above (SIGCONT, SIGSTOP, SIGIO) — is refused with
// OS64_SIG_ERR_BAD_SIGNAL, because a success for a signal nothing can send
// is a program waiting forever. (This paragraph said "nothing else is off
// limits" until Codex #29 rd16; the kernel's list is signal_is_known.)
//
// Returning the previous handler is deliberate: "install mine, remember
// theirs" is how a library that must not stomp its host behaves, and it is
// far easier to have from the first day than to retrofit.
static inline int64_t os64_signal_set_handler(int signo, os64_signal_fn handler)
{
    return (int64_t)os64_syscall2(SYSCALL_SIGNAL_HANDLER,
                                  (uint64_t)(int64_t)signo,
                                  (uint64_t)handler);
}

#endif // OS64_SIGNAL_H
