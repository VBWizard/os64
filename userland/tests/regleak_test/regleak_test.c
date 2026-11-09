// regleak_test — a syscall must not hand back registers the kernel didn't mean
// to hand back.
//
// THE PRINCIPLE, which is older and plainer than any ABI document: a boundary
// passes across what it intends to pass across, and nothing else. Everything
// else that survives the crossing is disclosure by accident — and accidents
// are exactly what nobody audits, because no single line looks wrong.
//
// What os64 was doing until 2026-08-24: the syscall exit path rebuilt RAX
// (the answer), RCX and R11 (consumed by SYSRETQ) and the user's own
// callee-saved registers, and left every other caller-saved register holding
// whatever kernel code happened to leave there. That is not vague residue —
// syscall.S loads R10 with `gs:[CLS_CURRENTTHREAD_OFFSET]` a dozen
// instructions before returning, so EVERY syscall handed ring 3 a live
// kernel heap pointer (the caller's own thread_t*). RDI, RDX, R8 and R9
// carried whatever the dispatcher's epilogue left, which nobody reasons
// about at all.
//
// Ring 3 is entitled to treat these as clobbered — they are caller-saved and
// libos64's stubs list them as such — so ZERO is every bit as legal an answer
// as garbage, and it is the answer that says nothing. Five `xor`s.
//
// Severity, stated honestly rather than inflated: os64 has no KASLR and its
// kernel base is a published constant, so a leaked kernel pointer buys an
// attacker far less here than on a system that hides its layout. That is an
// argument about how *much* it matters, not about whether a boundary should
// leak. It should not.
//
// THE PROBE is `taskid` — no arguments, no pointers, no buffers, the shortest
// path through the dispatcher there is. If even that comes back carrying
// kernel state, every longer syscall does too.
//
// Exit codes 0x2E600xx ("REG"; the step is the low byte):
//   0x02E60000  success
//   0x02E60001  a scratch register came back holding a KERNEL address
//   0x02E60002  a scratch register came back non-zero (weaker check; see below)

#include <stdint.h>
#include <stddef.h>

#include "os64/os64.h"
#include "os64/syscall_numbers.h"

#define STEP(n) (0x02E60000u | (uint32_t)(n))

// The canonical upper half — where every kernel address in os64 lives (the
// higher-half kernel at 0xffffffff80000000 and the HHDM alias both). A
// user-space value can never legitimately appear up here.
#define KERNEL_HALF 0xFFFF800000000000ULL

// Captured through MEMORY operands, never through registers: binding these to
// registers would let the compiler pick one of the very registers under test
// and overwrite the evidence before it is read.
static uint64_t gRdi, gRsi, gRdx, gR8, gR9, gR10;

static void die(uint32_t step, const char *why)
{
    os64_printf("regleak_test: %s\n", why);
    os64_serial_log(why);
    os64_exit(STEP(step));
}

static void check(const char *name, uint64_t v, int *dirty)
{
    if (v >= KERNEL_HALF) {
        os64_printf("regleak_test: %s came back as 0x%lx - a KERNEL address\n", name, v);
        os64_serial_log("regleak_test: a scratch register came back holding a kernel address");
        os64_exit(STEP(1));
    }
    if (v != 0) {
        os64_printf("regleak_test: %s came back as 0x%lx (not a kernel address, but not scrubbed)\n",
                    name, v);
        (*dirty)++;
    }
}

int main(void)
{
    uint64_t id;

    // taskid(), by hand, capturing the scratch registers the instant we are
    // back in ring 3 — before any C runs and overwrites them.
    __asm__ volatile(
        "syscall\n\t"
        "mov %[odi], rdi\n\t"
        "mov %[osi], rsi\n\t"
        "mov %[odx], rdx\n\t"
        "mov %[o8],  r8\n\t"
        "mov %[o9],  r9\n\t"
        "mov %[o10], r10\n\t"
        : "=a"(id),
          [odi] "=m"(gRdi), [osi] "=m"(gRsi), [odx] "=m"(gRdx),
          [o8]  "=m"(gR8),  [o9]  "=m"(gR9),  [o10] "=m"(gR10)
        : "a"((uint64_t)SYSCALL_TASKID)
        : "rcx", "r11", "memory");

    if (id == 0)
        die(1, "regleak_test: taskid() returned 0 - the probe syscall did not work");

    // THE SECURITY PROPERTY is the first check inside check(): no scratch
    // register may come back holding an address in the kernel's half. R10 is
    // the one that used to fail this every single time.
    int dirty = 0;
    check("rdi", gRdi, &dirty);
    check("rsi", gRsi, &dirty);
    check("rdx", gRdx, &dirty);
    check("r8",  gR8,  &dirty);
    check("r9",  gR9,  &dirty);
    check("r10", gR10, &dirty);

    // The stricter check, kept SEPARATE and second on purpose. Today the exit
    // path zeroes all six, so anything non-zero means the scrub was weakened
    // or a register was added to the path without being added to the scrub.
    // It is a maintenance tripwire rather than a security boundary — which is
    // why it has its own exit code, so a future reader can tell instantly
    // which of the two promises broke. (The day the RAX:RDX return pair lands,
    // RDX legitimately carries a status word and moves out of both lists.)
    if (dirty)
        die(2, "regleak_test: a scratch register survived the syscall un-scrubbed");

    os64_printf("regleak_test: syscall returned nothing it did not mean to (6 scratch registers clean)\n");
    os64_serial_log("regleak_test: no kernel state leaked through the syscall return");
    os64_exit(STEP(0));
    return 0;   // not reached
}
