// sigpipe_test — the SIGPIPE case no other fixture can reach: a HANDLER is
// installed, and delivering it is IMPOSSIBLE.
//
// IT PASSES BY DYING, like nx_test and malloctest's crime pair — and for the
// same reason: a tripwire nobody tests is a tripwire nobody knows is
// disconnected. Correct outcome is exit 141 (128 + 13, SIGPIPE's default
// action). SURVIVING is the failure.
//
// ── WHY THIS EXISTS (Codex #29 rd9) ─────────────────────────────────────────
//
// SIGPIPE became catchable in round 1, which created a state that had never
// existed before: a PENDING SIGPIPE on a live task, waiting to be delivered
// to a handler on the way out of write(). Delivery can fail — the frame is
// built on the victim's own stack, and §9's honest limit is that a stack
// which cannot hold the frame cannot receive the signal.
//
// When that happened, the kernel asked the WRONG QUESTION. It tested the
// signal against SIGNALS_TERMINATING, which is the set the CHECKPOINTS scan
// ("would a pending bit here stop this thread?"), and SIGPIPE is deliberately
// absent from it because an UNCAUGHT SIGPIPE dies at the write() site and
// never needs noticing later. So a handled SIGPIPE whose delivery failed was
// classified non-terminating, its pending bit was dropped, and write()
// returned OS64_INTERRUPTED — the process SURVIVED a signal whose default
// action is death, purely because its handler could not be reached. The
// question that should have been asked is "what happens when the handler
// cannot run?", which is about the DEFAULT ACTION: SIGNALS_DEFAULT_IS_DEATH.
//
// ── HOW IT FORCES THE FAILURE ───────────────────────────────────────────────
//
// The frame lands below RSP (red zone, then the frame, then 16-aligned), so
// an RSP with no writable user page beneath it makes the write fail. Getting
// one that is GUARANTEED unmapped rather than probably unmapped: map a region
// and immediately unmap it. os64 never reuses a region's VAs (DIVERGENCES §
// brk), so that address stays unmapped for the life of the process — which is
// a stronger promise than picking some low address and hoping.
//
// The pivot and the syscall are ONE asm block with nothing between them, for
// the reason CLAUDE.md gives about switching stacks: every C local after a
// `mov rsp` is reached through a stale frame pointer. All operands are loaded
// into registers BEFORE the pivot; RSP is saved to and restored from a GLOBAL
// (never the stack we are about to make a lie). The restore only executes if
// the kernel let us live, which is exactly the bug — so surviving lands us
// back on a real stack, able to report the failure properly instead of
// crashing in some unrelated way and looking like a different bug.
//
// Exit codes 0x51DE00xx ("SIgpipe DEath"; the step is the low byte):
//   141         success — the default action was applied (128 + 13)
//   0x51DE0001  the pipe could not be created
//   0x51DE0002  the SIGPIPE handler would not install
//   0x51DE0003  the scratch region could not be mapped/unmapped
//   0x51DE0004  FAIL — survived an undeliverable SIGPIPE (the bug is back)
//   0x51DE0005  FAIL — survived, AND the handler somehow ran

#include <stdint.h>
#include <stddef.h>

#include "os64/os64.h"
#include "os64/signal.h"
#include "os64/syscall_numbers.h"

#define STEP(n) (0x51DE0000u | (uint32_t)(n))

// Both of these MUST live outside the stack: the stack is about to point at
// unmapped memory, so anything the asm block touches through RSP is gone.
static volatile int gHandlerRan = 0;
static uint64_t     gSavedRsp   = 0;
static char         gPayload[64];

static void on_sigpipe(int signo)
{
    (void)signo;
    gHandlerRan = 1;
}

static void die(uint32_t step, const char *why)
{
    os64_printf("sigpipe_test: %s\n", why);
    os64_serial_log(why);
    os64_exit(STEP(step));
}

int main(void)
{
    // A pipe with nobody on the reading end — the classic SIGPIPE setup.
    int32_t fds[2];
    if (os64_pipe(fds) != 0)
        die(1, "sigpipe_test: could not create a pipe");
    os64_close(fds[0]);

    // A handler, so the kernel takes the "catchable" branch in write() and
    // publishes a PENDING SIGPIPE rather than dying on the spot. Without this
    // the fixture would prove nothing — the uncaught path was never in doubt.
    if (os64_signal_set_handler(OS64_SIGPIPE, on_sigpipe) < 0)
        die(2, "sigpipe_test: could not install the SIGPIPE handler");

    for (size_t i = 0; i < sizeof(gPayload); i++)
        gPayload[i] = 'x';

    // A guaranteed-unmapped address: map, then immediately give it back.
    void *scratch = os64_map(0x1000);
    if (scratch == NULL || os64_unmap(scratch) != 0)
        die(3, "sigpipe_test: could not make an unmapped scratch address");
    // Mid-region, so the whole frame (red zone + 64 bytes, rounded down) still
    // lands inside the range we just released rather than straddling out of it.
    uint64_t dead_rsp = (uint64_t)(uintptr_t)scratch + 0x800;

    os64_printf("sigpipe_test: writing to a reader-less pipe from an unusable stack...\n");
    os64_serial_log("sigpipe_test: writing to a reader-less pipe from an unusable stack");

    // ONE block: save RSP to a global, pivot, syscall, restore. Nothing
    // between the pivot and the syscall, and no C local touched across it.
    __asm__ volatile(
        "mov %[save], rsp\n\t"
        "mov rsp, %[dead]\n\t"
        "syscall\n\t"
        "mov rsp, %[save]\n\t"
        : [save] "+m"(gSavedRsp)
        : [dead] "r"(dead_rsp),
          "a"((uint64_t)SYSCALL_WRITE),
          "D"((uint64_t)(uint32_t)fds[1]),
          "S"((uint64_t)(uintptr_t)gPayload),
          "d"((uint64_t)sizeof(gPayload))
        : "rcx", "r11", "memory");

    // Only reached if the kernel did NOT apply SIGPIPE's default action —
    // which is the whole bug this fixture exists to catch.
    if (gHandlerRan)
        die(5, "sigpipe_test: FAIL - survived, and the handler ran on an unusable stack");
    die(4, "sigpipe_test: FAIL - survived an undeliverable SIGPIPE (default action not applied)");
    return 0;   // not reached; die() does not come back
}
