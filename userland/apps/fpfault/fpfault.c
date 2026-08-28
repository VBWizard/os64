// fpfault — a CPU exception raised from ring 3 kills the PROGRAM, not the OS.
//
// segv_test proves it for the page fault. This is the same promise for the
// exceptions a floating-point program can raise once the FPU slice turned
// them on, plus the oldest one of all:
//
//   fpfault xm   — unmask the SSE invalid-operation exception in MXCSR and
//                  compute 0.0/0.0. CR4.OSXMMEXCPT routes it to #XM
//                  (vector 19); the kernel must end us with exit 219.
//   fpfault mf   — unmask the x87 zero-divide exception in the control word
//                  and divide by zero on the x87 stack. CR0.NE routes it to
//                  #MF (vector 16) — and #MF is a trap taken at the NEXT
//                  x87 instruction, so one follows. Exit 216.
//   fpfault de   — integer divide by zero, #DE (vector 0). Exit 200.
//   fpfault ud   — an AVX instruction with CR4.OSXSAVE clear, #UD (vector
//                  6): the boot line's "present, not enabled" made real.
//                  Exit 206.
//
// The exit code is 200 + vector (simple_exceptions.c, user_exception_kill):
// the kill names the vector, and the fixture table expects that name. A
// program that survives its own fault reports 1 — the failure a harness
// would otherwise mistake for a pass.

#include "os64/os64.h"

// Not a pass and not a failure: the CPU (an emulator, in practice) recorded
// the exception but did not raise it. testrun treats this code as SKIP.
#define FPFAULT_NOT_TRAPPED 3

static int surviving(const char *what)
{
    os64_printf("fpfault: FAIL — %s and lived\n", what);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        os64_printf("usage: fpfault xm|mf|de|ud\n");
        return 2;
    }
    const char *which = argv[1];

    if (os64_streq(which, "xm"))
    {
        os64_printf("fpfault: unmasking SSE invalid-operation, then 0.0/0.0 — expect #XM, exit 219\n");
        uint32_t mxcsr = 0x1F80U & ~(1U << 7);   // clear IM: invalid-operation now traps
        uint32_t after = 0;
        __asm__ volatile(
            "ldmxcsr %[m]\n\t"
            "xorpd xmm0, xmm0\n\t"
            "divsd xmm0, xmm0\n\t"                 // 0/0 = invalid
            "stmxcsr %[a]\n\t"
            : [a] "=m"(after) : [m] "m"(mxcsr) : "xmm0", "memory");
        // An emulator that computes SSE but does not trap it (QEMU's TCG)
        // sets the IE flag and carries on. That is a fact about the CPU
        // under us, not about the kernel, and it must read as SKIP: the
        // kernel's side of #XM is exactly what the mf case proves.
        if (after & 1U)
        {
            os64_printf("fpfault: the invalid-operation flag was set but no #XM was raised — this CPU records SSE exceptions without trapping them (emulator?)\n");
            return FPFAULT_NOT_TRAPPED;
        }
        return surviving("divided 0.0 by 0.0 with the exception unmasked");
    }
    if (os64_streq(which, "mf"))
    {
        os64_printf("fpfault: unmasking x87 zero-divide, then 1.0/0.0 — expect #MF, exit 216\n");
        uint16_t cw = 0x037FU & ~(1U << 2);       // clear ZM: zero-divide now traps
        __asm__ volatile(
            "fldcw %[c]\n\t"
            "fld1\n\t"
            "fldz\n\t"
            "fdivp st(1), st\n\t"                  // 1/0 — the error is recorded here
            "fwait\n\t"                            // and reported HERE, at the next x87 instruction
            :: [c] "m"(cw) : "memory");
        return surviving("divided 1.0 by 0.0 on the x87 stack with the exception unmasked");
    }
    if (os64_streq(which, "de"))
    {
        os64_printf("fpfault: integer divide by zero — expect #DE, exit 200\n");
        // In asm, not C: division by zero is undefined behaviour, and GCC
        // uses the licence — `1 / zero` for an unsigned zero compiled to
        // `zero == 1` with no div instruction anywhere in the binary.
        uint64_t zero = 0;
        __asm__ volatile(
            "mov rax, 1\n\t"
            "xor rdx, rdx\n\t"
            "div %[z]\n\t"
            :: [z] "r"(zero) : "rax", "rdx", "cc");
        return surviving("divided by integer zero");
    }

    if (os64_streq(which, "ud"))
    {
        // The "present, not enabled" half of the boot line is a promise:
        // with CR4.OSXSAVE clear every VEX-encoded instruction is #UD, so
        // AVX cannot leave YMM state behind that FXSAVE would not carry.
        // vzeroupper is the smallest AVX instruction there is.
        os64_printf("fpfault: executing vzeroupper with XSAVE off — expect #UD, exit 206\n");
        __asm__ volatile("vzeroupper" ::: "memory");
        return surviving("executed an AVX instruction");
    }

    os64_printf("fpfault: unknown fault '%s' (xm, mf, de, ud)\n", which);
    return 2;
}
