// syscall_smoke.c — first true ring-3 fixture: SYSCALL/SYSRET round trip,
// write() to the console, and an explicit exit() syscall.
//
// The kernel launches this as /bin/syscall_smoke with isKernelTask=false, so
// unlike every fixture before it, this one actually runs at CPL 3 and crosses
// the privilege boundary through syscall_Enter/sysretq.  It exercises, in
// order:
//   1. SYSCALL_YIELD    — the simplest possible round trip (no pointer args,
//                         no CR3 switch).  If STAR/GDT are wrong, we die at
//                         the first sysret, right here, with nothing else in
//                         the picture.
//   2. SYSCALL_WRITE    — user pointer validated + copied under the user CR3,
//                         printed to the console under the kernel CR3.  The
//                         return value must equal the byte count sent.
//   3. SYSCALL_EXIT     — explicit exit; the kernel-side test asserts
//                         task->retVal == SMOKE_OK, which can ONLY have got
//                         there through the exit syscall (this _start never
//                         returns, so the ret-trampoline path can't set it).
//
// Failures return a distinct 0xE51Cxxxx code via exit() (or via `return`,
// where the exit trampoline turns RAX into the same retVal) so a regression
// pinpoints the step that broke.
//
// The syscall stubs below are the embryo of the userland syscall layer:
// os64's convention is RAX=number, args in RDI/RSI/RDX/R10/R8/R9, result in
// RAX; RCX/R11 are burned by the hardware and the kernel clobbers only what a
// C call would (see syscall.S), so a simple "syscall" instruction with the
// right register constraints is a complete stub.

#include <stdint.h>

// Must match kernel/include/syscall_numbers.h.
#define SYSCALL_YIELD      0
#define SYSCALL_EXIT       2
#define SYSCALL_WRITE      3
#define CONSOLE_OUT        1

#define SMOKE_OK           0x0005E00DUL   // "SGOOD" — all checks passed
#define FAIL_YIELD         0xE51C0001UL   // yield returned an error
#define FAIL_WRITE_RET     0xE51C0002UL   // write returned wrong byte count

static inline uint64_t os64_syscall0(uint64_t nr)
{
    uint64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr)
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

// Freestanding — no libc, so no strlen; the message length is compile-time.
static const char kMessage[] = "hello from ring 3 via write()\n";
#define MESSAGE_LEN (sizeof(kMessage) - 1)

unsigned long _start(unsigned long argc, char **argv, char **env)
{
    (void)argc;
    (void)argv;
    (void)env;

    // 1. Prove the bare SYSCALL/SYSRET round trip survives.
    if (os64_syscall0(SYSCALL_YIELD) != 0)
        return FAIL_YIELD;

    // 2. Prove pointer-carrying syscalls work end to end.
    uint64_t written = os64_syscall3(SYSCALL_WRITE, CONSOLE_OUT,
                                     (uint64_t)kMessage, MESSAGE_LEN);
    if (written != MESSAGE_LEN)
        os64_syscall3(SYSCALL_EXIT, FAIL_WRITE_RET, 0, 0);

    // 3. Explicit exit with the success sentinel.  Never returns.
    os64_syscall3(SYSCALL_EXIT, SMOKE_OK, 0, 0);

    // Unreachable — but if exit somehow returned, fall back to the trampoline
    // path so the kernel still sees a distinctive failure code.
    return 0xE51CDEADUL;
}
