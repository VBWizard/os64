// sigtest — the ring-3 fixture for signal handlers (2026-08-23).
//
// SIGNALS.md steps 2 and 3: a program can install a handler, and the kernel
// actually runs it. This fixture proves both halves and, deliberately, the
// awkward parts of each — that a REFUSED registration changes nothing, and
// that a syscall's return value survives a handler running in the middle of
// it, which is the quietest way the resume path could be wrong.
//
// Chris's os32 test app is the ancestor: install, raise, catch, carry on.
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
//   0x5160008  could not raise a signal at myself through /proc
//   0x5160009  the handler never ran (delivery is broken)
//   0x516000A  a syscall's return value did not survive delivery
//   0x516000B  a deliberate SIGSEGV was not caught (fault delivery is broken)
//   0x516000C  the SIGSEGV handler ran for the wrong signal
//   0x516000D  a shared-library-window handler address was wrongly rejected

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "os64/os64.h"
#include "os64/signal.h"

#define STEP(n) (0x05160000u | (uint32_t)(n))

static volatile int gCaught;

static void handler_a(int signo) { gCaught = signo; }
static void handler_b(int signo) { gCaught = signo + 1000; }

// The SIGSEGV handler is the acceptance test's finale, and it does NOT return:
// a SIGSEGV handler that returns resumes the faulting instruction and faults
// again, so a real one exits or longjmps. This one exits with SUCCESS — which
// is the whole fixture passing — after proving it caught the right signal.
static void on_segv(int signo)
{
    if (signo != OS64_SIGSEGV)
    {
        os64_serial_log("sigtest: SIGSEGV handler ran for the wrong signal");
        os64_exit(0x0516000Cu);
    }
    os64_printf("sigtest: caught a deliberate SIGSEGV and lived to tell — all correct\n");
    os64_serial_log("sigtest: caught a deliberate SIGSEGV and lived to tell — all correct");
    os64_exit(0x05160000u);   // success: the fixture ends HERE, inside the handler
}

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

    // IN RANGE IS NOT THE SAME AS REAL (Codex #29 rd13). The three checks
    // above only ever probed OUTSIDE the array, so a bare range check passed
    // them all while accepting every number INSIDE it — including numbers the
    // kernel has no enumerator for, and the scheduler's own markers.
    //
    // 3 is simply not a signal here: os64's numbering is POSIX's where POSIX
    // has one, and it has no 3. Registering it would leave a program waiting
    // for something nothing can ever send.
    if (os64_signal_set_handler(3, handler_a) != OS64_SIG_ERR_BAD_SIGNAL)
        die(5, "sigtest: signal 3 was accepted, and this kernel has no signal 3");

    // 25 is the one that mattered. It is SIGSLEEP — scheduler STATE that
    // happens to live in the same pending word signals do. While registration
    // was a range check, installing here meant the next time this program
    // slept, the kernel would deliver "signal 25" to the handler AND clear the
    // sleep marker on every thread of the task. Ring 3 editing kernel
    // scheduling state through an API that answered "success".
    if (os64_signal_set_handler(25, handler_a) != OS64_SIG_ERR_BAD_SIGNAL)
        die(5, "sigtest: a scheduler marker (25/SIGSLEEP) was accepted as a signal");

    // ...and the two refusals must stay DISTINGUISHABLE: SIGKILL is a real
    // signal you may not catch (UNCATCHABLE), 25 is not a signal at all
    // (BAD_SIGNAL). Collapsing them would send a caller looking in the wrong
    // place. (The SIGKILL half is checked above; this is the pairing.)
    if (os64_signal_set_handler(25, handler_a) == OS64_SIG_ERR_UNCATCHABLE)
        die(5, "sigtest: a non-signal was refused as if it were merely uncatchable");

    // A NUMBER IS NOT A SIGNAL UNTIL SOMETHING CAN SEND IT (Codex #29 rd14).
    // SIGCONT and SIGSTOP have numbers and no producer — job control is a
    // booked slice, not a shipped one — so accepting a handler for them would
    // leave a caller waiting forever for something nothing raises. Same
    // contract failure as accepting signal 3, arriving by a politer route.
    //
    // WHEN JOB CONTROL LANDS, THIS CHECK IS SUPPOSED TO FAIL. That is the
    // point of it: it will fail loudly on the first boot after SIGCONT gains
    // a producer, and whoever is doing that work flips these two lines to
    // expect success, in the same commit that adds them to signal_is_known.
    // A fixture that has to be edited by the person changing the behaviour is
    // a fixture that cannot silently rot.
    if (os64_signal_set_handler(OS64_SIGCONT, handler_a) != OS64_SIG_ERR_BAD_SIGNAL)
        die(5, "sigtest: SIGCONT was accepted, and nothing in this kernel can send it");
    if (os64_signal_set_handler(OS64_SIGSTOP, handler_a) != OS64_SIG_ERR_BAD_SIGNAL)
        die(5, "sigtest: SIGSTOP was accepted, and nothing in this kernel can send it");
    // SIGIO is the third one found this way, one round after the other two —
    // claimed alongside the POSIX numbers and never given a sender. The
    // kernel's list now names a PRODUCER for every signal it admits to
    // having, precisely so there is no fourth.
    if (os64_signal_set_handler(OS64_SIGIO, handler_a) != OS64_SIG_ERR_BAD_SIGNAL)
        die(5, "sigtest: SIGIO was accepted, and nothing in this kernel can send it");

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

    // A handler in the SHARED-LIBRARY WINDOW must be ACCEPTED. The whole
    // userland is dynamically linked, so a real handler is often a libos64
    // function, whose text lives at 0x7F00... — above the heap ceiling but
    // still valid user space. The kernel used to reject at TASK_HEAP_END and
    // refuse every library handler (Codex #29 rd5); the boundary is now the
    // canonical-user max. (We register a window ADDRESS to prove the range
    // check accepts it, then restore the default before it could ever be
    // delivered — the address need not point at real code for this check.)
    if (os64_signal_set_handler(OS64_SIGTERM,
                                (os64_signal_fn)0x00007F0000001000ULL) < 0)
        die(0x0D, "sigtest: a shared-library-window handler was wrongly rejected");
    (void)os64_signal_set_handler(OS64_SIGTERM, OS64_SIG_DEFAULT);

    // ── DELIVERY (step 3) ──────────────────────────────────────────────────
    // Send ourselves a signal and prove the handler actually runs, that the
    // interrupted work resumes afterwards, and that the syscall which was in
    // flight still returns its own answer.
    gCaught = 0;
    if (os64_signal_set_handler(OS64_SIGINT, handler_a) < 0)
        die(1, "sigtest: could not install the delivery handler");

    // Aim SIGINT at ourselves the way kill(1) does — a write to our own
    // /proc/<pid>/ctl. (There is no kill SYSCALL: signalling is a file
    // operation here, which is the Plan 9 shape /proc was built for.)
    {
        char path[64];
        os64_snprintf(path, sizeof(path), "/proc/%ld/ctl", (long)os64_taskid());
        int64_t ctl = os64_open(path, "w");
        if (ctl < 0)
            die(8, "sigtest: could not open my own /proc ctl");
        // The bit lands on every thread of this task during THIS write. Its
        // delivery happens on the way out of a syscall — which means the very
        // write that raised it is the one that carries the handler.
        if (os64_write((int32_t)ctl, "interrupt", 9) < 0)
            die(8, "sigtest: could not signal myself");
        os64_close((int32_t)ctl);
    }

    // One ordinary syscall. Its return value must survive the handler running
    // in the middle of it — that is the whole point of saving RAX in the
    // frame, and a taskid whose answer came back mangled would prove the
    // resume path wrong in the quietest possible way.
    int64_t id_before = os64_taskid();
    int64_t id_after  = os64_taskid();

    if (gCaught != OS64_SIGINT)
        die(9, "sigtest: the handler never ran");
    if (id_before != id_after || id_after <= 0)
        die(10, "sigtest: a syscall's return value did not survive delivery");

    // And the program is still alive, which is the entire point: SIGINT's
    // default action is death, and installing a handler replaced it.
    if (os64_signal_set_handler(OS64_SIGINT, OS64_SIG_DEFAULT) != (int64_t)(uintptr_t)handler_a)
        die(3, "sigtest: the handler was not still installed after delivery");

    os64_printf("sigtest: registration, refusals, delivery and resume all correct\n");
    os64_serial_log("sigtest: registration, refusals, delivery and resume all correct");

    // ── SIGSEGV: the acceptance test (SIGNALS.md §9) ───────────────────────
    // Install a handler, then fault ON PURPOSE, and prove the handler runs.
    // Chris's os32 test app is the direct ancestor — fault, catch, brag. The
    // handler (on_segv) exits with success, so reaching any line after the
    // deref means the fault was NOT delivered.
    if (os64_signal_set_handler(OS64_SIGSEGV, on_segv) < 0)
        die(1, "sigtest: could not install the SIGSEGV handler");

    volatile int *wild = (volatile int *)0;   // NULL — guaranteed unmapped (page-0 guard)
    *wild = 0x1234;                            // <<< SIGSEGV is raised HERE

    // Only reached if the store above did NOT fault into the handler.
    die(0x0B, "sigtest: a deliberate SIGSEGV was not caught");
    return 0;   // not reached
}
