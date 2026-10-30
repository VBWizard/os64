// stackprobe — a runaway recursion dies IN THE STACK GUARD, named for what
// it is, and never takes the OS with it.
//
// Two halves make that true, and this fixture proves them together:
//
//   - userland is built -fstack-clash-protection, so a frame larger than a
//     page is lowered a page at a time and each page is touched on the way
//     down. RSP cannot step over the unmapped guard below the thread's
//     stack; the first probe into it faults there. Without the flag, the
//     64KB frame here would leap the guard in one `sub rsp` and die
//     wherever the arithmetic put it — sometimes the guard, usually not.
//   - the kernel (simple_exceptions.c, stack_guard_side) recognizes a fault
//     in the faulting thread's own guard and heads the report "Stack
//     overflow" instead of "Segmentation fault ... access to unmapped
//     address", with a forensics line placing the fault against the stack.
//
// Exit 139 is still the verdict — it is a segmentation fault — and that is
// what testrun checks. The headline is checked by reading the serial log,
// where it is grep-able: "Stack overflow: stackprobe".
//
// A program that survives its own runaway reports 1, the failure a harness
// would otherwise mistake for a pass.

#include "os64/os64.h"

// A depth no stack can reach — 2^40 frames of 64KB is 64 petabytes — behind
// a volatile so GCC cannot see that the recursion never ends (it refuses to
// compile one it can prove infinite, -Winfinite-recursion).
static volatile uint64_t s_floor = 1ULL << 40;

// A frame of 64KB, well past the 16KB guard, so a leap would be a leap.
// Written at both ends and read back after the call so the compiler can
// neither shrink the array nor turn the recursion into a loop (a tail call
// would reuse the frame and never overflow anything).
static __attribute__((noinline)) uint64_t deeper(uint64_t n)
{
    volatile uint8_t buf[65536];
    buf[0] = (uint8_t)n;
    buf[sizeof(buf) - 1] = (uint8_t)(n >> 8);
    if (n >= s_floor)
        return n;
    uint64_t below = deeper(n + 1);
    return below + buf[0] + buf[sizeof(buf) - 1];
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    os64_printf("stackprobe: recursing with a 64KB frame until the stack ends — expect \"Stack overflow\", exit 139\n");
    uint64_t sum = deeper(0);
    os64_printf("stackprobe: FAIL — recursed %lu deep and lived\n", sum);
    return 1;
}
