#ifndef OS64_ABI_SYSCALL_H
#define OS64_ABI_SYSCALL_H

// Raw os64 syscall stubs — the register contract, encoded once.
//
// This is pure ABI vocabulary (the fence rule, ABI.md): number in RAX, args
// in RDI/RSI/RDX/R10/R8/R9, result in RAX. The hardware burns RCX (return
// RIP) and R11 (RFLAGS); the kernel additionally clobbers only what a plain
// C call would, so these constraint lists make a bare `syscall` instruction
// a complete stub. Anything friendlier than "registers in, register out"
// belongs in libos64, not here.
//
// Graduated from kernel/test/elf/syscall_smoke.c — "the embryo of the
// userland syscall layer", now hatched.

#ifndef __ASSEMBLER__

#include <stdint.h>
#include "os64/syscall_numbers.h"

static inline uint64_t os64_syscall0(uint64_t nr)
{
    uint64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline uint64_t os64_syscall1(uint64_t nr, uint64_t a0)
{
    uint64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline uint64_t os64_syscall2(uint64_t nr, uint64_t a0, uint64_t a1)
{
    uint64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline uint64_t os64_syscall3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2)
{
    uint64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
                     : "rcx", "r11", "memory");
    return ret;
}

// arg3 rides in R10 (not RCX — the hardware owns RCX during syscall), so it
// needs an explicit register variable; "c" would be wrong here.
static inline uint64_t os64_syscall4(uint64_t nr, uint64_t a0, uint64_t a1,
                                     uint64_t a2, uint64_t a3)
{
    register uint64_t r10 __asm__("r10") = a3;
    uint64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline uint64_t os64_syscall6(uint64_t nr, uint64_t a0, uint64_t a1,
                                     uint64_t a2, uint64_t a3, uint64_t a4,
                                     uint64_t a5)
{
    register uint64_t r10 __asm__("r10") = a3;
    register uint64_t r8  __asm__("r8")  = a4;
    register uint64_t r9  __asm__("r9")  = a5;
    uint64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}

#endif // __ASSEMBLER__
#endif // OS64_ABI_SYSCALL_H
