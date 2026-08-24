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

#define OS64_SIGHUP    1    // your terminal hung up (tty_shell_departed)
#define OS64_SIGINT    2    // Ctrl+C
#define OS64_SIGKILL   9    // uncatchable, by design — see below
#define OS64_SIGSEGV   11   // you touched memory that isn't yours
#define OS64_SIGPIPE   13   // you are writing into a pipe nobody reads
#define OS64_SIGTERM   15   // the machine is going down; finish up
#define OS64_SIGCONT   18
#define OS64_SIGSTOP   19
// 28 is SIGWINCH everywhere and is RESERVED, not defined: the terminal-resize
// slice gives it something to mean, and a number claimed early is one nobody
// has to renegotiate.
#define OS64_SIGIO     29

#define OS64_SIGNAL_COUNT 32

// WHAT AN INTERRUPTED CALL ANSWERS. A blocking call whose thread ran a
// handler returns this instead of its normal result: the wait was cut short,
// nothing was accomplished, and the caller decides what to do about it.
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
// decline to die would have no last resort. Nothing else is off limits —
// including SIGSEGV, though a handler for it runs on the stack that just
// faulted, so if the STACK is what went wrong there is nowhere to put the
// frame and the thread dies as it always did.
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
