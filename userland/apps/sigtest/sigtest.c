// sigtest — the ring-3 fixture for signal handler registration (2026-08-23).
//
// SIGNALS.md step 2 landed the registration syscall; delivery is step 3. So
// this proves exactly what exists and claims nothing about what does not: a
// handler can be installed, the previous one comes back, the default can be
// restored, and the refusals refuse.
//
// It will GROW a second half the day delivery lands — raise SIGSEGV, catch it,
// print, die — which is Chris's os32 test app reborn and the acceptance test
// for step 3. The scaffolding is written to make that an addition rather than
// a rewrite.
//
// Exit codes 0x51600xx ("SIGO"; the step is the low byte):
//   0x5160000  success
//   0x5160001  installing a handler failed
//   0x5160002  the previous handler was not reported back
//   0x5160003  restoring the default did not report the handler it replaced
//   0x5160004  SIGKILL was allowed to be caught (it must never be)
//   0x5160005  an out-of-range signal was accepted
//   0x5160006  a kernel-space handler address was accepted
//   0x5160007  a handler did not survive being read back

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "os64/os64.h"
#include "os64/signal.h"

#define STEP(n) (0x05160000u | (uint32_t)(n))

static volatile int gCaught;

static void handler_a(int signo) { gCaught = signo; }
static void handler_b(int signo) { gCaught = signo + 1000; }

static void die(uint32_t step, const char *why)
{
    os64_printf("sigtest: %s\n", why);
    os64_serial_log(why);
    os64_exit(STEP(step));
}

int main(void)
{
    // ── install, and get the old one back ──────────────────────────────────
    // There was no handler, so the previous one is the default (0).
    int64_t prev = os64_signal_set_handler(OS64_SIGTERM, handler_a);
    if (prev < 0)
        die(1, "sigtest: could not install a SIGTERM handler");
    if (prev != (int64_t)OS64_SIG_DEFAULT)
        die(2, "sigtest: a fresh signal did not report the DEFAULT handler");

    // Replacing it must hand back exactly what was there — the property that
    // lets a library install its own without stomping its host's.
    prev = os64_signal_set_handler(OS64_SIGTERM, handler_b);
    if (prev != (int64_t)(uintptr_t)handler_a)
        die(2, "sigtest: replacing a handler did not report the previous one");

    // ── the handler is the TASK's, so a read-back must see it ──────────────
    // (Setting the same value returns it, which is the only query this ABI
    // has and is deliberate — see os64/signal.h.)
    prev = os64_signal_set_handler(OS64_SIGTERM, handler_b);
    if (prev != (int64_t)(uintptr_t)handler_b)
        die(7, "sigtest: the installed handler did not survive a read-back");

    // ── back to the kernel's default ───────────────────────────────────────
    prev = os64_signal_set_handler(OS64_SIGTERM, OS64_SIG_DEFAULT);
    if (prev != (int64_t)(uintptr_t)handler_b)
        die(3, "sigtest: restoring the default did not report the handler it replaced");
    prev = os64_signal_set_handler(OS64_SIGTERM, OS64_SIG_DEFAULT);
    if (prev != (int64_t)OS64_SIG_DEFAULT)
        die(3, "sigtest: the default did not stick");

    // ── the refusals ───────────────────────────────────────────────────────
    // SIGKILL is the answer to a program that has stopped answering. If this
    // check ever passes, the kernel has lost its last resort.
    if (os64_signal_set_handler(OS64_SIGKILL, handler_a) != OS64_SIG_ERR_UNCATCHABLE)
        die(4, "sigtest: SIGKILL accepted a handler — the last resort is gone");

    if (os64_signal_set_handler(0, handler_a) != OS64_SIG_ERR_BAD_SIGNAL)
        die(5, "sigtest: signal 0 was accepted");
    if (os64_signal_set_handler(OS64_SIGNAL_COUNT, handler_a) != OS64_SIG_ERR_BAD_SIGNAL)
        die(5, "sigtest: an out-of-range signal was accepted");
    if (os64_signal_set_handler(-1, handler_a) != OS64_SIG_ERR_BAD_SIGNAL)
        die(5, "sigtest: a negative signal was accepted");

    // A handler in the higher half would have the CPU attempt kernel text at
    // CPL 3. It faults harmlessly, but being refused by NAME beats finding
    // out three steps later.
    if (os64_signal_set_handler(OS64_SIGTERM,
                                (os64_signal_fn)0xffffffff80000000ULL)
        != OS64_SIG_ERR_BAD_HANDLER)
        die(6, "sigtest: a kernel-space handler address was accepted");

    // A refused call must change NOTHING.
    prev = os64_signal_set_handler(OS64_SIGTERM, OS64_SIG_DEFAULT);
    if (prev != (int64_t)OS64_SIG_DEFAULT)
        die(6, "sigtest: a refused install changed the handler anyway");

    (void)gCaught;   // step 3's business
    os64_printf("sigtest: registration, replacement and refusals all correct\n");
    os64_serial_log("sigtest: registration, replacement and refusals all correct");
    os64_exit(STEP(0));
    return 0;   // not reached
}
