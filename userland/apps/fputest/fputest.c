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
// The signal frame is tested at the same time, on BOTH delivery paths,
// and the fixture knows which one each delivery took. The handler wipes
// every XMM register, the x87 stack, and both control registers — but
// first it reads the x87 control word it was handed, which names the
// interrupted thread (every thread's is distinct), so each delivery is
// counted against the thread that took it:
//
//   §10, the scheduler's own delivery: a HELPER PROCESS (this program,
//   re-spawned with --helper) sends the task SIGINT from outside. The
//   spinning workers make no syscalls, so a delivery to one of them can
//   only have come through the scheduler visit and the full frame. The
//   fixture requires at least one such delivery.
//
//   §5, the syscall-exit delivery: the pinger thread loads its own pattern,
//   issues the ctl write as a RAW syscall instruction inside the same asm
//   block, and checks its registers on the far side. SIGINT published by
//   that very write is delivered to the writing thread at the syscall's
//   exit — Codex #35 pointed out that this is where a self-sent signal
//   ALWAYS lands, which is why the earlier version of this fixture, whose
//   sender held no pattern, was proving nothing about frames at all.
//
// Then the hostile case. The frame lives on the program's own stack, and
// sigreturn restores MXCSR out of it with fxrstor — in the KERNEL. An MXCSR
// with a reserved bit set makes fxrstor #GP, so the kernel masks the value
// with the CPU's own MXCSR_MASK first. The last phase writes 0xFFFFFFFF
// into the frame's MXCSR slot from inside a handler and returns: the OS
// must survive, and what comes back must be a legal MXCSR.
//
// ONE ASM BLOCK PER CHECK, on purpose. The state is loaded and checked
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
#define FPUTEST_NO_SIGNALS       0xF0DE0006   // no delivery ever reached a spinning worker (§10 untested)
#define FPUTEST_NO_CTL           0xF0DE0007
#define FPUTEST_MXCSR_CLOBBERED  0xF0DE0008
#define FPUTEST_X87CW_CLOBBERED  0xF0DE0009
#define FPUTEST_HIGHXMM_CLOBBERED 0xF0DE000A
#define FPUTEST_FORGE_NO_SIGNAL  0xF0DE000B
#define FPUTEST_FORGE_BAD_MXCSR  0xF0DE000C
#define FPUTEST_NO_HELPER        0xF0DE000D   // could not spawn the external sender
#define FPUTEST_NO_SELF_SIGNALS  0xF0DE000E   // the pinger's own syscall exits never delivered (§5 untested)

#define WORKERS      4
#define PATTERNS     (WORKERS + 2)   // + the main thread + the pinger: every thread its own
#define MAIN_PATTERN   WORKERS
#define PINGER_PATTERN (WORKERS + 1)
#define RUN_MS       2000
#define CHUNK_ITERS  1000000UL
#define HELPER_SENDS 60              // ~2s of external SIGINTs at 30ms
#define HELPER_GAP_MS 30

// Which checks failed inside a chunk — one bit each, OR'd across chunks.
#define BAD_XMM7    (1U << 0)
#define BAD_ST0     (1U << 1)
#define BAD_MXCSR   (1U << 2)
#define BAD_X87CW   (1U << 3)
#define BAD_HIGHXMM (1U << 4)

// One pattern per THREAD — workers, main, and the pinger. Every field
// differs between any two of them, so a cross-thread leak cannot pass by
// luck (Codex #35: main used to share worker 3's, and a leak between
// exactly those two would have been invisible). The x87 control words are
// ALL distinct on purpose: the handler identifies the interrupted thread
// by the one it finds loaded.
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
static pattern_t kPatterns[PATTERNS] = {
    { 0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL, 0, 0, 1.0000001, 0x1F80U | (0U << 13), 0x037FU },
    { 0xDEADBEEFCAFEBABEULL, 0x1122334455667788ULL, 0, 0, 2.7182818, 0x1F80U | (1U << 13), 0x077FU },   // round down
    { 0xA5A5A5A55A5A5A5AULL, 0x0F0F0F0FF0F0F0F0ULL, 0, 0, 3.1415926, 0x1F80U | (2U << 13), 0x0A7FU },   // round up, double precision
    { 0x8000000000000001ULL, 0x7FFFFFFFFFFFFFFFULL, 0, 0, 0.0000042, 0x1F80U | (3U << 13), 0x0F7FU },   // toward zero
    { 0xC0FFEE0000C0FFEEULL, 0x0BADF00D0BADF00DULL, 0, 0, 6.0221408, 0x1F80U | (1U << 13), 0x027FU },   // main: round down, single
    { 0x5EED5EED5EED5EEDULL, 0x9E3779B97F4A7C15ULL, 0, 0, 1.6180339, 0x1F80U | (2U << 13), 0x0E7FU },   // pinger: round up, toward-zero x87
};

static volatile int64_t gHandlerRuns;
static volatile int64_t gRunsByPattern[PATTERNS];   // who was interrupted, by control word
static volatile int64_t gRunsUnknown;               // a thread with no pattern loaded
static volatile bool    gStop;

// Wipe everything the frame is supposed to protect. Every XMM register
// zeroed, the x87 stack emptied and its control word reset, MXCSR reset.
// If sigreturn restores from the frame, the interrupted thread never
// notices; if it restores nothing, every check in the next iteration fails.
//
// Before the wipe: the live file still belongs to the interrupted thread
// (the frame is a COPY), so the control word read here says whose turn it
// was. That is how the fixture knows a delivery reached a spinning worker.
static void clobber_handler(int signo)
{
    (void)signo;
    static const uint32_t reset_mxcsr = 0x1F80U;
    uint16_t cw;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    bool known = false;
    for (int i = 0; i < PATTERNS; i++)
        if (kPatterns[i].x87cw == cw) { gRunsByPattern[i]++; known = true; }
    if (!known)
        gRunsUnknown++;
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

// The load and the check, as text, so the spin loop and the syscall probe
// share ONE copy of each. Operand names are the ones both asm statements
// bind below.
#define FPU_LOAD_ASM \
        "ldmxcsr [%[pat] + 40]\n\t"            /* MXCSR <- this thread's rounding mode */ \
        "fldcw   [%[pat] + 44]\n\t"            /* x87 CW <- this thread's rounding/precision */ \
        "movdqu xmm7, [%[pat]]\n\t"      /* XMM7 <- pattern */ \
        "movdqa xmm8, xmm7\n\t"  "paddq xmm8, xmm7\n\t"    /* 2x */ \
        "movdqa xmm9, xmm8\n\t"  "paddq xmm9, xmm7\n\t"    /* 3x */ \
        "movdqa xmm10, xmm9\n\t" "paddq xmm10, xmm7\n\t"   /* 4x */ \
        "movdqa xmm11, xmm10\n\t" "paddq xmm11, xmm7\n\t"  /* 5x */ \
        "movdqa xmm12, xmm11\n\t" "paddq xmm12, xmm7\n\t"  /* 6x */ \
        "movdqa xmm13, xmm12\n\t" "paddq xmm13, xmm7\n\t"  /* 7x */ \
        "movdqa xmm14, xmm13\n\t" "paddq xmm14, xmm7\n\t"  /* 8x */ \
        "movdqa xmm15, xmm14\n\t" "paddq xmm15, xmm7\n\t"  /* 9x  (sum 44x) */ \
        "fld qword ptr [%[pat] + 32]\n\t"     /* st(0) <- constant */

// Local labels 2-9 belong to the check; a caller's own labels must avoid them.
#define FPU_CHECK_ASM \
        /* XMM7 against the pattern */ \
        "movdqu [%[scr]], xmm7\n\t" \
        "mov rax, [%[scr]]\n\t" \
        "cmp rax, [%[pat]]\n\t" \
        "jne 2f\n\t" \
        "mov rax, [%[scr] + 8]\n\t" \
        "cmp rax, [%[pat] + 8]\n\t" \
        "je 3f\n\t" \
        "2:\n\t" \
        "or %[bad], %[bxmm]\n\t" \
        "3:\n\t" \
        /* XMM8..15 summed against 44x the pattern */ \
        "movdqa xmm6, xmm8\n\t" \
        "paddq xmm6, xmm9\n\t"  "paddq xmm6, xmm10\n\t" "paddq xmm6, xmm11\n\t" \
        "paddq xmm6, xmm12\n\t" "paddq xmm6, xmm13\n\t" "paddq xmm6, xmm14\n\t" "paddq xmm6, xmm15\n\t" \
        "movdqu [%[scr]], xmm6\n\t" \
        "mov rax, [%[scr]]\n\t" \
        "cmp rax, [%[pat] + 16]\n\t" \
        "jne 5f\n\t" \
        "mov rax, [%[scr] + 8]\n\t" \
        "cmp rax, [%[pat] + 24]\n\t" \
        "je 6f\n\t" \
        "5:\n\t" \
        "or %[bad], %[bhigh]\n\t" \
        "6:\n\t" \
        /* st(0) against the constant (fst, not fstp: it stays loaded) */ \
        "fst qword ptr [%[scr] + 16]\n\t" \
        "mov rax, [%[scr] + 16]\n\t" \
        "cmp rax, [%[pat] + 32]\n\t" \
        "je 7f\n\t" \
        "or %[bad], %[bst]\n\t" \
        "7:\n\t" \
        /* MXCSR against what we loaded — the whole word, flags included */ \
        "stmxcsr [%[scr] + 24]\n\t" \
        "mov eax, [%[scr] + 24]\n\t" \
        "cmp eax, [%[pat] + 40]\n\t" \
        "je 8f\n\t" \
        "or %[bad], %[bmx]\n\t" \
        "8:\n\t" \
        /* x87 control word */ \
        "fnstcw [%[scr] + 28]\n\t" \
        "mov ax, [%[scr] + 28]\n\t" \
        "cmp ax, [%[pat] + 44]\n\t" \
        "je 9f\n\t" \
        "or %[bad], %[bcw]\n\t" \
        "9:\n\t"

// Two base registers, not eight: the pattern's fields and the scratch
// slots are reached by offset, because the syscall probe below needs six
// more GPRs than an eight-pointer operand list leaves on x86-64. The
// offsets are asserted against the struct, so a reordered field breaks
// the build rather than the comparison.
_Static_assert(__builtin_offsetof(pattern_t, x87)    == 32, "pattern_t.x87 is read at +32");
_Static_assert(__builtin_offsetof(pattern_t, mxcsr)  == 40, "pattern_t.mxcsr is read at +40");
_Static_assert(__builtin_offsetof(pattern_t, x87cw)  == 44, "pattern_t.x87cw is read at +44");
_Static_assert(__builtin_offsetof(pattern_t, sum_lo) == 16, "pattern_t.sum_lo is read at +16");
// scratch[4]: +0..15 an XMM spill, +16 the st(0) store, +24 MXCSR, +28 the x87 CW.
#define FPU_ASM_OPERANDS(p, scratch) \
          [pat] "r"(p), [scr] "r"(scratch), \
          [bxmm] "i"(BAD_XMM7), [bst] "i"(BAD_ST0), [bmx] "i"(BAD_MXCSR), \
          [bcw] "i"(BAD_X87CW), [bhigh] "i"(BAD_HIGHXMM)

#define FPU_ASM_CLOBBERS \
          "rax", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", \
          "xmm12", "xmm13", "xmm14", "xmm15", "memory", "cc"

// Load the pattern, then spin CHUNK_ITERS times comparing what the
// registers hold against what was loaded. Returns the BAD_* bits.
static uint32_t run_chunk(const pattern_t *p)
{
    uint32_t bad = 0;
    uint64_t scratch[4];
    __asm__ volatile(
        FPU_LOAD_ASM
        "mov rcx, %[iters]\n\t"
        "1:\n\t"
        FPU_CHECK_ASM
        "dec rcx\n\t"
        "jnz 1b\n\t"
        "fstp st(0)\n\t"                 // leave the x87 stack as we found it
        : [bad] "+r"(bad)
        : FPU_ASM_OPERANDS(p, scratch), [iters] "i"(CHUNK_ITERS)
        : "rcx", FPU_ASM_CLOBBERS);
    return bad;
}

// Load the pattern, make ONE syscall — the ctl write that publishes SIGINT
// to this very thread — and check on the far side. The `syscall` is inline
// so nothing but the kernel stands between load and check: the frame is
// built from the live file at the syscall's exit, the handler wipes it,
// and sigreturn must put it back before this instruction stream resumes.
//
// The three syscall arguments travel in a struct behind ONE pointer: the
// check already binds eight register operands, and eight plus three plus
// the six registers the syscall clobbers is more than x86-64 has.
typedef struct { uint64_t handle; const char *text; uint64_t length; } syscall_args_t;

static uint32_t run_syscall_checked(const pattern_t *p, int64_t handle,
                                    const char *text, uint64_t length)
{
    uint32_t bad = 0;
    uint64_t scratch[4];
    syscall_args_t args = { (uint64_t)handle, text, length };
    __asm__ volatile(
        FPU_LOAD_ASM
        "mov rax, %[nr]\n\t"
        "mov rdi, [%[a]]\n\t"
        "mov rsi, [%[a] + 8]\n\t"
        "mov rdx, [%[a] + 16]\n\t"
        "syscall\n\t"
        FPU_CHECK_ASM
        "fstp st(0)\n\t"
        : [bad] "+r"(bad)
        : FPU_ASM_OPERANDS(p, scratch),
          [nr] "i"(SYSCALL_WRITE), [a] "r"(&args)
        // A syscall clobbers what a C call clobbers (ABI.md § register
        // contract): every caller-saved GPR, not just the hardware's
        // RCX/R11 — the kernel zeroes them so nothing of its own leaks out.
        // Name them all, or the compiler parks an operand in R10 and the
        // first store after `syscall` goes through a null pointer.
        : "rcx", "r11", "rdi", "rsi", "rdx", "r8", "r9", "r10", FPU_ASM_CLOBBERS);
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

static int64_t open_ctl(uint64_t taskid)
{
    char path[64];
    os64_snprintf(path, sizeof(path), "/proc/%lu/ctl", taskid);
    return os64_open(path, "w");
}

// The §5 probe: every 20ms, the pinger sends ITSELF SIGINT through a raw
// syscall with a pattern loaded, and checks. Returns the BAD_* bits, or
// FPUTEST_NO_CTL.
static int64_t pinger(void *arg)
{
    (void)arg;
    int64_t h = open_ctl(os64_taskid());
    if (h < 0)
        return FPUTEST_NO_CTL;
    uint32_t bad = 0;
    while (!gStop)
    {
        bad |= run_syscall_checked(&kPatterns[PINGER_PATTERN], h, "interrupt", 9);
        os64_sleep(20);
    }
    os64_close((int32_t)h);
    return (int64_t)bad;
}

// The §10 sender: a separate PROCESS, so its syscalls are its own and the
// only threads that can take the signal are the fixture's. `fputest
// --helper <taskid>` sends HELPER_SENDS SIGINTs, HELPER_GAP_MS apart.
static int helper_main(const char *taskid_text)
{
    uint64_t taskid = 0;
    for (const char *c = taskid_text; *c >= '0' && *c <= '9'; c++)
        taskid = taskid * 10 + (uint64_t)(*c - '0');
    int64_t h = open_ctl(taskid);
    if (h < 0)
        return 1;
    for (int i = 0; i < HELPER_SENDS; i++)
    {
        os64_write((int32_t)h, "interrupt", 9);
        os64_sleep(HELPER_GAP_MS);
    }
    os64_close((int32_t)h);
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
    if (argc == 3 && os64_streq(argv[1], "--helper"))
        return helper_main(argv[2]);

    for (int i = 0; i < PATTERNS; i++)
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

    char taskid_text[24];
    os64_snprintf(taskid_text, sizeof(taskid_text), "%lu", os64_taskid());
    char *const helper_argv[] = { "/bin/fputest", "--helper", taskid_text, NULL };
    int64_t helper = os64_spawn("/bin/fputest", helper_argv);
    if (helper < 0)
    {
        os64_printf("fputest: could not spawn the external sender\n");
        return FPUTEST_NO_HELPER;
    }

    // The main thread works too, on its own pattern — six threads on
    // however many cores there are guarantees preemption and sharing.
    os64_ticks_t start, now;
    os64_ticks(&start);
    uint32_t bad = 0;
    do {
        bad |= run_chunk(&kPatterns[MAIN_PATTERN]);
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
    bad |= (uint32_t)ping_result;
    int32_t helper_code = 0;
    while (os64_wait(helper, &helper_code) == OS64_INTERRUPTED)
        ;   // a late SIGINT ends the wait early; wait again

    int64_t worker_runs = 0;
    for (int i = 0; i < WORKERS; i++)
        worker_runs += gRunsByPattern[i];
    os64_printf("fputest: %ld handler runs — %ld on spinning workers (scheduler-delivered), %ld on main, %ld on the pinger's own syscall (syscall-exit), %ld unidentified\n",
                gHandlerRuns, worker_runs, gRunsByPattern[MAIN_PATTERN],
                gRunsByPattern[PINGER_PATTERN], gRunsUnknown);
    os64_printf("fputest: bad checks: xmm7 %s, xmm8-15 %s, st(0) %s, mxcsr %s, x87cw %s\n",
                (bad & BAD_XMM7) ? "YES" : "none", (bad & BAD_HIGHXMM) ? "YES" : "none",
                (bad & BAD_ST0) ? "YES" : "none", (bad & BAD_MXCSR) ? "YES" : "none",
                (bad & BAD_X87CW) ? "YES" : "none");
    if (bad & BAD_XMM7)    return FPUTEST_XMM_CLOBBERED;
    if (bad & BAD_HIGHXMM) return FPUTEST_HIGHXMM_CLOBBERED;
    if (bad & BAD_ST0)     return FPUTEST_X87_CLOBBERED;
    if (bad & BAD_MXCSR)   return FPUTEST_MXCSR_CLOBBERED;
    if (bad & BAD_X87CW)   return FPUTEST_X87CW_CLOBBERED;
    // A pass that never exercised a path must say so rather than claim it.
    if (worker_runs == 0)
    {
        os64_printf("fputest: no SIGINT ever reached a spinning worker — the scheduler-delivered frame went untested\n");
        return FPUTEST_NO_SIGNALS;
    }
    if (gRunsByPattern[PINGER_PATTERN] == 0)
    {
        os64_printf("fputest: the pinger's own syscalls never delivered — the syscall-exit frame went untested\n");
        return FPUTEST_NO_SELF_SIGNALS;
    }

    // Now the forgery. Only this thread is left, so the signal is ours.
    // Delivery rides the ctl write's own syscall exit (§5), so the handler
    // has run by the time the write returns; the sleep is a backstop.
    os64_signal_set_handler(OS64_SIGINT, forge_handler);
    int64_t h = open_ctl(os64_taskid());
    if (h < 0)
        return FPUTEST_NO_CTL;
    os64_write((int32_t)h, "interrupt", 9);
    os64_close((int32_t)h);
    for (int i = 0; i < 50 && gForgeRuns == 0; i++)
        os64_sleep(10);
    if (gForgeRuns == 0)
    {
        os64_printf("fputest: the forging handler never ran\n");
        return FPUTEST_FORGE_NO_SIGNAL;
    }
    // Still here: the kernel survived the fxrstor. What did it hand back?
    // Every bit above 17 is reserved on every CPU (bit 17 only on AMD, and
    // the mask knows); a legal MXCSR has none of them set.
    uint32_t mxcsr_now;
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr_now));
    os64_printf("fputest: forged MXCSR 0xFFFFFFFF came back as 0x%x\n", mxcsr_now);
    if (mxcsr_now & 0xFFFC0000U)
        return FPUTEST_FORGE_BAD_MXCSR;
    return FPUTEST_OK;
}
