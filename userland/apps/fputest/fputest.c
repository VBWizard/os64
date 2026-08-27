// fputest — the floating-point register file survives everything the
// scheduler can do to a thread, and a program cannot use its own signal
// frame to hurt the kernel.
//
// The FPU slice made x87/MMX/SSE state part of a thread's context: saved
// on every switch, restored on every load, snapshotted into every signal
// frame. Each of those is a place a register can silently come back wrong,
// and "silently" is the whole hazard — a float program with a clobbered
// XMM register does not crash, it computes the wrong picture. Worse than a
// wrong DATA register is a wrong CONTROL register: if MXCSR's rounding mode
// does not switch with the thread, every result is off by an ULP and
// nothing anywhere notices.
//
// So: several threads, each holding a DIFFERENT pattern in XMM7, a
// different arithmetic ladder in XMM8..XMM15, a different constant on the
// x87 stack, a different rounding mode in MXCSR and a different one in the
// x87 control word — all spinning on the same cores, each checking all
// five every iteration. Preemption puts thread B's state on the core
// between two of thread A's checks; if the scheduler's save/restore has a
// hole, A sees B's. Migration across cores is covered by the same test,
// because the state travels in the thread struct, not the core.
//
// The signal frame is tested at the same time: one thread keeps sending
// the task SIGINT, and the handler deliberately wipes every XMM register,
// the x87 stack, and both control registers. Whichever thread the kernel
// hands the signal to is mid-check when the handler runs — its state must
// come back from the frame intact, or that thread's check fails. The spin
// loop makes no syscalls, so the handler arrives by the scheduler's own
// delivery path (SIGNALS.md §10, the full frame); the sender thread's own
// syscalls exercise the dispatcher path (§5).
//
// Then the hostile case. The frame lives on the program's own stack, and
// sigreturn restores MXCSR out of it with fxrstor — in the KERNEL. An MXCSR
// with a reserved bit set makes fxrstor #GP, so the kernel masks the value
// with the CPU's own MXCSR_MASK first. The last phase writes 0xFFFFFFFF
// into the frame's MXCSR slot from inside a handler and returns: the OS
// must survive, and what comes back must be a legal MXCSR.
//
// ONE ASM BLOCK PER CHUNK, on purpose. The state is loaded and checked
// inside a single asm statement, so no C code — and no function call — sits
// between load and check. XMM registers are caller-saved in the SysV ABI:
// a callee is ALLOWED to clobber XMM7, and a test that called os64_ticks
// between load and check would be testing the ABI, not the kernel. A chunk
// is a million iterations, far longer than a scheduler tick, so every
// preemption lands inside one.
//
// Exit codes name the failed step: 0xF0DE0000 is a pass.

#include "os64/os64.h"

#define FPUTEST_OK               0xF0DE0000
#define FPUTEST_NO_START         0xF0DE0001
#define FPUTEST_BAD_JOIN         0xF0DE0002
#define FPUTEST_XMM_CLOBBERED    0xF0DE0003
#define FPUTEST_X87_CLOBBERED    0xF0DE0004
#define FPUTEST_NO_HANDLER       0xF0DE0005
#define FPUTEST_NO_SIGNALS       0xF0DE0006
#define FPUTEST_NO_CTL           0xF0DE0007
#define FPUTEST_MXCSR_CLOBBERED  0xF0DE0008
#define FPUTEST_X87CW_CLOBBERED  0xF0DE0009
#define FPUTEST_HIGHXMM_CLOBBERED 0xF0DE000A
#define FPUTEST_FORGE_NO_SIGNAL  0xF0DE000B
#define FPUTEST_FORGE_BAD_MXCSR  0xF0DE000C

#define WORKERS      4
#define RUN_MS       2000
#define CHUNK_ITERS  1000000UL

// Which checks failed inside a chunk — one bit each, OR'd across chunks.
#define BAD_XMM7    (1U << 0)
#define BAD_ST0     (1U << 1)
#define BAD_MXCSR   (1U << 2)
#define BAD_X87CW   (1U << 3)
#define BAD_HIGHXMM (1U << 4)

// One pattern per worker. Every field differs between any two threads, so
// a cross-thread leak cannot pass by luck.
typedef struct {
    uint64_t lo, hi;      // XMM7; XMM8..15 hold 2x..9x of it per lane
    uint64_t sum_lo, sum_hi;  // 44x per lane — the XMM8..15 checksum
    double   x87;         // st(0)
    uint32_t mxcsr;       // all exceptions masked, a distinct rounding mode
    uint16_t x87cw;       // all exceptions masked, distinct rounding + precision
} pattern_t;

// MXCSR: 0x1F80 is every exception masked; bits 13-14 are the rounding
// mode. x87 CW: 0x037F is masked/extended/nearest; bits 10-11 rounding,
// 8-9 precision.
static pattern_t kPatterns[WORKERS] = {
    { 0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL, 0, 0, 1.0000001, 0x1F80U | (0U << 13), 0x037FU },
    { 0xDEADBEEFCAFEBABEULL, 0x1122334455667788ULL, 0, 0, 2.7182818, 0x1F80U | (1U << 13), 0x077FU },   // round down, single precision
    { 0xA5A5A5A55A5A5A5AULL, 0x0F0F0F0FF0F0F0F0ULL, 0, 0, 3.1415926, 0x1F80U | (2U << 13), 0x0A7FU },   // round up, double precision
    { 0x8000000000000001ULL, 0x7FFFFFFFFFFFFFFFULL, 0, 0, 0.0000042, 0x1F80U | (3U << 13), 0x0F7FU },   // toward zero, extended
};

static volatile int64_t gHandlerRuns;
static volatile bool    gStop;

// Wipe everything the frame is supposed to protect. Every XMM register
// zeroed, the x87 stack emptied and its control word reset, MXCSR reset.
// If sigreturn restores from the frame, the interrupted thread never
// notices; if it restores nothing, every check in the next iteration fails.
static void clobber_handler(int signo)
{
    (void)signo;
    static const uint32_t reset_mxcsr = 0x1F80U;
    __asm__ volatile(
        "pxor xmm0, xmm0\n\t"  "pxor xmm1, xmm1\n\t"  "pxor xmm2, xmm2\n\t"  "pxor xmm3, xmm3\n\t"
        "pxor xmm4, xmm4\n\t"  "pxor xmm5, xmm5\n\t"  "pxor xmm6, xmm6\n\t"  "pxor xmm7, xmm7\n\t"
        "pxor xmm8, xmm8\n\t"  "pxor xmm9, xmm9\n\t"  "pxor xmm10, xmm10\n\t" "pxor xmm11, xmm11\n\t"
        "pxor xmm12, xmm12\n\t" "pxor xmm13, xmm13\n\t" "pxor xmm14, xmm14\n\t" "pxor xmm15, xmm15\n\t"
        "fninit\n\t"
        "ldmxcsr %[m]\n\t"
        :: [m] "m"(reset_mxcsr) : "memory");
    gHandlerRuns++;
}

// Load the pattern, then spin CHUNK_ITERS times comparing what the
// registers hold against what was loaded. Returns the BAD_* bits.
static uint32_t run_chunk(const pattern_t *p)
{
    uint32_t bad = 0;
    uint64_t scratch[2];
    double   x87_out;
    uint32_t mxcsr_out;
    uint16_t cw_out;
    __asm__ volatile(
        "ldmxcsr [%[mx]]\n\t"            // MXCSR <- this thread's rounding mode
        "fldcw   [%[cw]]\n\t"            // x87 CW <- this thread's rounding/precision
        "movdqu xmm7, [%[pat]]\n\t"      // XMM7 <- pattern
        "movdqa xmm8, xmm7\n\t"  "paddq xmm8, xmm7\n\t"    // 2x
        "movdqa xmm9, xmm8\n\t"  "paddq xmm9, xmm7\n\t"    // 3x
        "movdqa xmm10, xmm9\n\t" "paddq xmm10, xmm7\n\t"   // 4x
        "movdqa xmm11, xmm10\n\t" "paddq xmm11, xmm7\n\t"  // 5x
        "movdqa xmm12, xmm11\n\t" "paddq xmm12, xmm7\n\t"  // 6x
        "movdqa xmm13, xmm12\n\t" "paddq xmm13, xmm7\n\t"  // 7x
        "movdqa xmm14, xmm13\n\t" "paddq xmm14, xmm7\n\t"  // 8x
        "movdqa xmm15, xmm14\n\t" "paddq xmm15, xmm7\n\t"  // 9x  (sum 44x)
        "fld qword ptr [%[x87]]\n\t"     // st(0) <- constant
        "mov rcx, %[iters]\n\t"
        "1:\n\t"
        // XMM7 against the pattern
        "movdqu [%[scr]], xmm7\n\t"
        "mov rax, [%[scr]]\n\t"
        "cmp rax, [%[pat]]\n\t"
        "jne 2f\n\t"
        "mov rax, [%[scr] + 8]\n\t"
        "cmp rax, [%[pat] + 8]\n\t"
        "je 3f\n\t"
        "2:\n\t"
        "or %[bad], %[bxmm]\n\t"
        "3:\n\t"
        // XMM8..15 summed against 44x the pattern
        "movdqa xmm6, xmm8\n\t"
        "paddq xmm6, xmm9\n\t"  "paddq xmm6, xmm10\n\t" "paddq xmm6, xmm11\n\t"
        "paddq xmm6, xmm12\n\t" "paddq xmm6, xmm13\n\t" "paddq xmm6, xmm14\n\t" "paddq xmm6, xmm15\n\t"
        "movdqu [%[scr]], xmm6\n\t"
        "mov rax, [%[scr]]\n\t"
        "cmp rax, [%[pat] + 16]\n\t"
        "jne 5f\n\t"
        "mov rax, [%[scr] + 8]\n\t"
        "cmp rax, [%[pat] + 24]\n\t"
        "je 6f\n\t"
        "5:\n\t"
        "or %[bad], %[bhigh]\n\t"
        "6:\n\t"
        // st(0) against the constant (fst, not fstp: it stays loaded)
        "fst qword ptr [%[out]]\n\t"
        "mov rax, [%[out]]\n\t"
        "cmp rax, [%[x87]]\n\t"
        "je 7f\n\t"
        "or %[bad], %[bst]\n\t"
        "7:\n\t"
        // MXCSR against what we loaded — the whole word, flags included
        // (nothing here raises one, so none may appear)
        "stmxcsr [%[mxo]]\n\t"
        "mov eax, [%[mxo]]\n\t"
        "cmp eax, [%[mx]]\n\t"
        "je 8f\n\t"
        "or %[bad], %[bmx]\n\t"
        "8:\n\t"
        // x87 control word
        "fnstcw [%[cwo]]\n\t"
        "mov ax, [%[cwo]]\n\t"
        "cmp ax, [%[cw]]\n\t"
        "je 9f\n\t"
        "or %[bad], %[bcw]\n\t"
        "9:\n\t"
        "dec rcx\n\t"
        "jnz 1b\n\t"
        "fstp st(0)\n\t"                 // leave the x87 stack as we found it
        : [bad] "+r"(bad)
        : [pat] "r"(p), [x87] "r"(&p->x87), [mx] "r"(&p->mxcsr), [cw] "r"(&p->x87cw),
          [iters] "i"(CHUNK_ITERS), [scr] "r"(scratch), [out] "r"(&x87_out),
          [mxo] "r"(&mxcsr_out), [cwo] "r"(&cw_out),
          [bxmm] "i"(BAD_XMM7), [bst] "i"(BAD_ST0), [bmx] "i"(BAD_MXCSR),
          [bcw] "i"(BAD_X87CW), [bhigh] "i"(BAD_HIGHXMM)
        : "rax", "rcx", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11",
          "xmm12", "xmm13", "xmm14", "xmm15", "memory", "cc");
    return bad;
}

static int64_t worker(void *arg)
{
    const pattern_t *p = &kPatterns[(int64_t)arg];
    uint32_t bad = 0;
    while (!gStop)
        bad |= run_chunk(p);
    return (int64_t)bad;
}

static bool send_self_sigint(void)
{
    char path[64];
    os64_snprintf(path, sizeof(path), "/proc/%lu/ctl", os64_taskid());
    int64_t h = os64_open(path, "w");
    if (h < 0)
        return false;
    os64_write((int32_t)h, "interrupt", 9);
    os64_close((int32_t)h);
    return true;
}

// Keeps poking the task with SIGINT until the workers are told to stop.
// Writing "interrupt" to our own /proc/<id>/ctl is how kill(1) does it.
static int64_t pinger(void *arg)
{
    (void)arg;
    while (!gStop)
    {
        if (!send_self_sigint())
            return FPUTEST_NO_CTL;
        os64_sleep(20);
    }
    return 0;
}

// ── The hostile frame ──────────────────────────────────────────────────────
//
// The stub CALLs the handler with RSP pointing AT the frame (task_exit_asm.S),
// so at -O0 — push rbp; mov rbp, rsp — the frame is rbp + 16. Its FXSAVE
// image starts at +64 (signal_frame_t, signals.h) and MXCSR is at +24 inside
// that: frame + 88. Write all ones there and let sigreturn deal with it.
#define FRAME_MXCSR_OFFSET (64 + 24)

static volatile int64_t gForgeRuns;

static void forge_handler(int signo)
{
    (void)signo;
    uint8_t *frame = (uint8_t *)__builtin_frame_address(0) + 16;
    *(volatile uint32_t *)(frame + FRAME_MXCSR_OFFSET) = 0xFFFFFFFFU;
    gForgeRuns++;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    for (int i = 0; i < WORKERS; i++)
    {
        kPatterns[i].sum_lo = kPatterns[i].lo * 44ULL;   // paddq wraps per lane, so does this
        kPatterns[i].sum_hi = kPatterns[i].hi * 44ULL;
    }

    if (os64_signal_set_handler(OS64_SIGINT, clobber_handler) < 0)
    {
        os64_printf("fputest: could not install the SIGINT handler\n");
        return FPUTEST_NO_HANDLER;
    }

    int64_t handles[WORKERS];
    for (int64_t i = 0; i < WORKERS; i++)
    {
        handles[i] = os64_thread(worker, (void *)i);
        if (handles[i] < 0)
        {
            os64_printf("fputest: could not start worker %ld\n", i);
            return FPUTEST_NO_START;
        }
    }
    int64_t ping = os64_thread(pinger, NULL);
    if (ping < 0)
        return FPUTEST_NO_START;

    // The main thread works too, on the last pattern — six threads on
    // however many cores there are guarantees preemption and sharing.
    os64_ticks_t start, now;
    os64_ticks(&start);
    uint32_t bad = 0;
    do {
        bad |= run_chunk(&kPatterns[WORKERS - 1]);
        os64_ticks(&now);
    } while ((now.ticks - start.ticks) * 1000 / now.per_second < RUN_MS);
    gStop = true;

    for (int64_t i = 0; i < WORKERS; i++)
    {
        int64_t wbad = -1;
        if (os64_thread_join((int32_t)handles[i], &wbad) < 0 || wbad < 0)
        {
            os64_printf("fputest: join of worker %ld failed\n", i);
            return FPUTEST_BAD_JOIN;
        }
        bad |= (uint32_t)wbad;
    }
    int64_t ping_result = 0;
    os64_thread_join((int32_t)ping, &ping_result);
    if (ping_result == FPUTEST_NO_CTL)
    {
        os64_printf("fputest: could not open /proc/self/ctl to send SIGINT\n");
        return FPUTEST_NO_CTL;
    }

    os64_printf("fputest: %ld handler runs; bad checks: xmm7 %s, xmm8-15 %s, st(0) %s, mxcsr %s, x87cw %s\n",
                gHandlerRuns,
                (bad & BAD_XMM7) ? "YES" : "none", (bad & BAD_HIGHXMM) ? "YES" : "none",
                (bad & BAD_ST0) ? "YES" : "none", (bad & BAD_MXCSR) ? "YES" : "none",
                (bad & BAD_X87CW) ? "YES" : "none");
    if (bad & BAD_XMM7)    return FPUTEST_XMM_CLOBBERED;
    if (bad & BAD_HIGHXMM) return FPUTEST_HIGHXMM_CLOBBERED;
    if (bad & BAD_ST0)     return FPUTEST_X87_CLOBBERED;
    if (bad & BAD_MXCSR)   return FPUTEST_MXCSR_CLOBBERED;
    if (bad & BAD_X87CW)   return FPUTEST_X87CW_CLOBBERED;
    // A pass with no handler run tested only the scheduler, and must say so
    // rather than claim the frame was proven too.
    if (gHandlerRuns == 0)
    {
        os64_printf("fputest: no SIGINT was ever delivered — the signal frame went untested\n");
        return FPUTEST_NO_SIGNALS;
    }

    // Now the forgery. Only this thread is left, so the signal is ours.
    // Delivery rides the ctl write's own syscall exit (§5), so the handler
    // has run by the time send_self_sigint returns; the sleep is a backstop.
    os64_signal_set_handler(OS64_SIGINT, forge_handler);
    if (!send_self_sigint())
        return FPUTEST_NO_CTL;
    for (int i = 0; i < 50 && gForgeRuns == 0; i++)
        os64_sleep(10);
    if (gForgeRuns == 0)
    {
        os64_printf("fputest: the forging handler never ran\n");
        return FPUTEST_FORGE_NO_SIGNAL;
    }
    // Still here: the kernel survived the fxrstor. What did it hand back?
    // Every bit above 15 is reserved on every CPU (bit 17 only on AMD, and
    // the mask knows); a legal MXCSR has none of them set.
    uint32_t mxcsr_now;
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr_now));
    os64_printf("fputest: forged MXCSR 0xFFFFFFFF came back as 0x%x\n", mxcsr_now);
    if (mxcsr_now & 0xFFFC0000U)
        return FPUTEST_FORGE_BAD_MXCSR;
    return FPUTEST_OK;
}
