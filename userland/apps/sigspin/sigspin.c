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

static void on_interrupt(int signo)
{
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

    for (;;)
    {
        spins++;    // the whole program. No kernel, no boundary, no mercy.

        if (gInterrupts != seen)
        {
            seen = gInterrupts;
            os64_printf("  *** caught signal %d MID-SPIN (after %lu spins) — still here (%d of %d) ***\n",
                        gLastSignal, (unsigned long)spins, seen, GIVE_IN_AFTER);
            if (seen >= GIVE_IN_AFTER)
            {
                os64_printf("\n  all right, all right. The core is yours again.\n");
                return 0;
            }
        }
    }
}
