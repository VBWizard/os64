// sigspin — the stubborn spinning core, caught mid-spin (SIGNALS.md §10).
//
// sigdemo proves a handler runs when a signal interrupts a SYSCALL (a sleep).
// This is the other program, the one §10 was designed for: a loop that never
// enters the kernel at all. There is no syscall return to arm a handler on —
// the ONLY way in is the scheduler, which catches the spin where the timer
// interrupted it, builds the full-register frame, and runs the handler with
// the spin none the wiser. Before §10, this program was reachable only by
// SIGKILL; Ctrl+C with a handler installed did nothing at all.
//
// The spin body makes NO syscalls — that is the entire point. The prints
// happen only AFTER a catch (the report, not the experiment), so a Ctrl+C
// pressed while it counts is, overwhelmingly, delivered to a spinning
// thread by the scheduler. The kernel's own receipt is the DEBUG_SIGNALS
// line "signal_deliver_to_regs: sigspin runs handler ... (spinner resumes
// ...)" in the log — grep for "spinner" if you want the proof in writing.
//
// Three Ctrl+Cs and it gives in, sigdemo's own courtesy: a program that
// catches SIGINT forever is a program you can only kill.

#include <stdint.h>
#include <stdbool.h>

#include "os64/os64.h"
#include "os64/signal.h"

#define GIVE_IN_AFTER 3

// `volatile` for the same reason as sigdemo's: the handler writes these and
// the spin reads them, and nothing tells the compiler that can happen. Here
// it is doubly load-bearing — without it the spin's poll of gInterrupts
// would be hoisted out of the loop and the catches would never be noticed.
static volatile int gInterrupts;
static volatile int gLastSignal;
// The flags the handler was ENTERED with. §10 delivery is the only path that
// can be tested this way — see the DF note in main().
static volatile uint64_t gHandlerFlags;

static void on_interrupt(int signo)
{
    // First thing, before the compiler can emit anything that touches the
    // direction flag: what state were we handed?
    uint64_t f;
    __asm__ volatile("pushfq\n\tpop %0" : "=r"(f) :: "memory");
    gHandlerFlags = f;

    gLastSignal = signo;
    gInterrupts++;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (os64_signal_set_handler(OS64_SIGINT, on_interrupt) < 0)
    {
        os64_hprintf(OS64_STDERR, "sigspin: this kernel cannot install a handler\n");
        return 1;
    }

    os64_printf("sigspin: spinning with NO syscalls. Press Ctrl+C.\n");
    os64_printf("         (only the scheduler can reach me out here)\n\n");

    int seen = 0;
    // Also volatile so the increment is a real store — a loop the optimizer
    // can prove empty is a loop it may fold, and an honest spin does work.
    volatile uint64_t spins = 0;

    // SPIN WITH THE DIRECTION FLAG SET (2026-08-24, Codex #29 rd11).
    //
    // This program is the only place that can test one particular promise.
    // A handler must be entered with DF clear — otherwise a string operation
    // inside it runs backward — and the kernel clears it on all three
    // delivery paths. But §5 and §9 hand the flags back through a syscall or
    // exception frame, while §10 (this path) resumes through the scheduler's
    // per-core mp_isrSaved* mirror, which is a SEPARATE copy. Round 10
    // cleared DF in thread->regs and forgot the mirror, so on the continue
    // path the fix did nothing at all — and nothing noticed, because df_test
    // is delivered through §5.
    //
    // A spin that makes no syscalls is the only way to guarantee §10
    // delivery, which is exactly what this program already is. So: set DF,
    // spin, and let the handler report what it was entered with. `std` is
    // safe here because the loop below is pure arithmetic on a volatile —
    // no calls, no string operations — until a catch is noticed, and the
    // first thing done then is `cld`.
    __asm__ volatile("std" ::: "cc");

    for (;;)
    {
        spins++;    // the whole program. No kernel, no boundary, no mercy.

        if (gInterrupts != seen)
        {
            // Back to a sane direction before ANY library call runs.
            __asm__ volatile("cld" ::: "cc");
            seen = gInterrupts;

            // The §10 promise, checked where it is checkable.
            if (gHandlerFlags & (1ULL << 10))
                os64_printf("  !!! sigspin: the handler was entered with DF SET (flags 0x%lx) !!!\n",
                            (unsigned long)gHandlerFlags);
            else
                os64_printf("  (handler entered with DF clear, as it must be)\n");

            os64_printf("  *** caught signal %d MID-SPIN (after %lu spins) — still here (%d of %d) ***\n",
                        gLastSignal, (unsigned long)spins, seen, GIVE_IN_AFTER);
            if (seen >= GIVE_IN_AFTER)
            {
                os64_printf("\n  all right, all right. The core is yours again.\n");
                return 0;   // DF already clear — we cld'd on the way in here
            }

            // Re-arm the flag for the NEXT catch. Without this only the first
            // interrupt would be tested with DF set and the remaining two
            // would quietly check nothing — the vacuous-assertion trap again,
            // hiding inside a loop this time.
            __asm__ volatile("std" ::: "cc");
        }
    }
}
